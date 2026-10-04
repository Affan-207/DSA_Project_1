// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

/// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {
        if (count >= MAX_STACK_DEPTH) {
            throw runtime_error("Stack Full");
        }
        else {
            Node* newNode = new Node();
            newNode->data = val;
            newNode->next = top;
            top = newNode;
            count++;
        }

        // pushes the value on the stack if max limit is not reached yet.
    }
    T pop()
    {
        if (isEmpty()) {
            throw runtime_error("Stack Empty");
        }
        else {
            T val = top->data;
            Node* temp = top;
            top = top->next;
            delete temp;
            count--;
            return val;
        }
        // pop the top value on the stack
    }
    T &peek()
    {
        if (isEmpty()) {
            throw runtime_error("Stack Empty");
        }

        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return top == nullptr;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* curr = top;
        int32_t i = 0;

        while (curr != nullptr && i < maxLen) {
            out[i] = curr->data;
            curr = curr->next;
            i++;
        }
        return i;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {

        TimelineNode* new_node = new TimelineNode();
        new_node->next = nullptr;
        new_node->prev = nullptr;
        new_node->data = s;

        if (head==nullptr) {
            head = tail = new_node;
        }
        else {
            tail->next = new_node;
            new_node->prev = tail;
            tail = new_node;
        }
        stepCount++;

        // add record in the timeline
    }
    TimelineNode *begin()
    {
        return head;

    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    while (getline(in, out)) {

        if (!out.empty() ) return true;

    }
    return false;
    // reads the next nonblank line
}
string firstWord(const string &line)
{
    int idx = line.find(' ');

    return line.substr(0, idx);
    // returns first word from the input string
}
string secondWord(const string &line)
{
    int idx1 = line.find(" ");

    int idx2 = line.find(" ", idx1 + 1);

    return line.substr(idx1 + 1, idx2 - idx1 - 1); 
    
    // returns the second word
}
bool validateProgram(const char *sourcePath)
{
    ifstream fin(sourcePath);

    if (!fin) return false;
    else {
        bool inside_func = false;
        bool main_exists = false;
        string line;

        while (readSourceLine(fin, line)) {
            if (firstWord(line) == "func") {
                if (inside_func = true) {
                    return false; // wwe aint treating nested functions
                }

                inside_func = true;

                if (secondWord(line) == "main") main_exists = true;
            }
            else if (firstWord(line) == "func_end") {
                if (inside_func == false) return false;

                inside_func = false;
            }
        }
        return (inside_func == false && main_exists == true);
    }


    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t initial_pos = ftell(f);
    fwrite(&offsetField, sizeof(int16_t), 1, f);

    int32_t size = text.size();
    fwrite(&size, sizeof(int32_t), 1, f);

    fwrite(text.data(), sizeof(char), size, f);

    return initial_pos;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    int64_t offset = 0;
    if (fread(&offset, sizeof(int64_t), 1, f) != 1) {
        return -1;
    }

    int32_t size = 0;
    if (fread(&size, sizeof(int32_t), 1, f) != 1) {
        return -1;
    }

    outText.resize(size);

    if (size > 0) {
        if (fread(&outText[0], sizeof(char), size, f) != (size_t)size) {
            return -1;
        }
    }

    return offset;

    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream sourceFile(sourcePath);
    if (!sourceFile.is_open()) {
        cerr << "File not open";
        return -1;
    }

    FILE* resolveFile = fopen(resolveBinPath, "wb+");
    if (!resolveFile) {
        cerr << "File not open";
        sourceFile.close();
        return -1;
    }

    string line;
    int64_t currentOffset = 0;
    int64_t mainOffset = -1;


    // --- Pass 1: Write records and collect symbols/patches ---
    while (readSourceLine(sourceFile, line)) {
        string keyword = firstWord(line);

        // We prepare the record payload. Initially, for CALL instructions, 
        // the offset field is a placeholder (0) until we patch it later.
        int64_t recordStart = currentOffset;

        // Write the record to resolve.bin: [offset (8B)][size (4B)][string data]
        // Note: For now, we write 0 as the stored record offset prefix, or we can write recordStart.
        // Let's use writeResolveRecord helper format.
        int64_t offsetFieldLoc = currentOffset; // position where the 8-byte offset sits

        // Let's write the record using a helper logic directly or via writeResolveRecord
        fseek(resolveFile, currentOffset, SEEK_SET);

        // Write placeholder/actual offset field (8 bytes)
        int64_t fileOffsetPlaceholder = 0;
        fwrite(&fileOffsetPlaceholder, sizeof(int64_t), 1, resolveFile);

        // Write size of string (4 bytes)
        int32_t stringSize = static_cast<int32_t>(line.size());
        fwrite(&stringSize, sizeof(int32_t), 1, resolveFile);

        // Write string data
        fwrite(line.c_str(), 1, stringSize, resolveFile);

        // Update current file offset tracker
        currentOffset += sizeof(int64_t) + sizeof(int32_t) + stringSize;
        
        // Inspect keywords for symbol resolution
        if (keyword == "func") {
            string funcName = secondWord(line);
            if (funcCount < MAX_FUNCS) {
                funcArray[funcCount].funcName = funcName;
                funcArray[funcCount++].byteOffsetInResolveBin = recordStart;
            }
            if (funcName == "main") {
                mainOffset = recordStart;
            }
        }
        else if (keyword == "call") {
            string targetName = secondWord(line);
            if (patchCount < MAX_PATCHES) {
                // The offset field that needs patching is right at recordStart
                patches[patchCount].byteOffsetOfOffsetField = recordStart;
                patches[patchCount++].targetFuncName = targetName;
            }
        }
    }

    // --- Pass 2: Patch the CALL instructions ---
    for (int32_t i = 0; i < patchCount; i++) {
        int64_t targetOffset = -1;

        // Find the target function's byte offset from funcArray
        for (int32_t j = 0; j < funcCount; j++) {
            if (funcArray[j].funcName == patches[i].targetFuncName) {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset != -1) {
            // Seek to the exact offset field location of that call instruction in resolve.bin
            fseek(resolveFile, patches[i].byteOffsetOfOffsetField, SEEK_SET);
            // Overwrite the 8-byte placeholder with the real target byte offset
            fwrite(&targetOffset, sizeof(int64_t), 1, resolveFile);
        }
        else {
            cerr << "Error: Unresolved function call target: " << patches[i].targetFuncName << endl;
            fclose(resolveFile);
            return -1;
        }
    }

    fclose(resolveFile);

    if (mainOffset == -1) {
        cerr << "Error: 'main' function not found in source program." << endl;
        return -1;
    }

    return mainOffset;

    sourceFile.close();

     

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    stringstream ss(line);
    string word;
    int32_t tokenCount = 0;

    while (ss >> word && tokenCount < maxTokens) {
        if (tokenCount == 0) {
            tokens[tokenCount].type = KEYWORD; 
        }
        else if (tokenCount == 1) {
            tokens[tokenCount].type = IDENTIFIER;  
        }
        else {
            tokens[tokenCount].type = PARAM;       
        }

        tokens[tokenCount].text = word;            
        tokenCount++;
    }
    return tokenCount;
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);

    return s;
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* f = fopen(resolveBinPath, "rb"); // Open resolve.bin for binary reading
    if (!f) return;

    Stack<Frame> callStack; // Initialize the live execution call stack

    // 1. Create and push the initial 'main' frame onto the stack
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    // Record the very first timeline step at the start of main
    timeline.record(buildSnapshot(callStack));

    // 2. Jump straight to main's offset in the binary file
    fseek(f, mainOffset, SEEK_SET);

    string line;
    int64_t nextOffset = mainOffset;

    // 3. Main execution loop reading record by record
    while (true) {
        int64_t currentRecordOffset = ftell(f);

        // Read the binary record (8-byte offset, 4-byte size, string text)
        int64_t storedOffsetField;
        if (fread(&storedOffsetField, sizeof(int64_t), 1, f) != 1) break;

        int32_t stringSize;
        if (fread(&stringSize, sizeof(int32_t), 1, f) != 1) break;

        string rawLine(stringSize, ' ');
        if (fread(&rawLine[0], 1, stringSize, f) != static_cast<size_t>(stringSize)) break;

        // Tokenize the line to understand what command to run
        Token tokens[MAX_TOKENS];
        int32_t tokenCount = tokenizeLine(rawLine, tokens, MAX_TOKENS);
        if (tokenCount == 0) continue;

        string keyword = tokens[0].text;

        // 4. Handle instructions based on keywords
        if (keyword == "func_end") {
            // End of function: pop the top frame off the call stack
            if (!callStack.isEmpty()) {
                callStack.pop();
            }
            // If the stack is completely empty, main has finished executing!
            if (callStack.isEmpty()) {
                break;
            }
        }
        else if (keyword == "call") {
            // For a call instruction, the 8-byte storedOffsetField holds the target file address!
            // We push a new frame onto the stack and jump to that file position.
            Frame newFrame;
            newFrame.func_name = tokens[1].text;
            newFrame.localCount = 0;
            callStack.push(newFrame);

            fseek(f, storedOffsetField, SEEK_SET); // Jump file pointer to the target function
        }
        else if (keyword == "set") {
            // Example: set x 10 -> create or update a local variable
            if (!callStack.isEmpty() && tokenCount >= 3) {
                Frame& currentFrame = callStack.peek();
                string varName = tokens[1].text;
                int32_t val = stoi(tokens[2].text);

                // Look for existing local variable to update it
                bool found = false;
                for (int32_t i = 0; i < currentFrame.localCount; i++) {
                    if (currentFrame.locals[i].name == varName) {
                        currentFrame.locals[i].value = val;
                        found = true;
                        break;
                    }
                }
                // If not found, add it as a new local variable
                if (!found && currentFrame.localCount < MAX_VARS_PER_FRAME) {
                    currentFrame.locals[currentFrame.localCount].name = varName;
                    currentFrame.locals[currentFrame.localCount].value = val;
                    currentFrame.localCount++;
                }
            }
        }
        // (You can similarly implement add, sub, mul, div operations here by updating variable values)

        // 5. After every single instruction, capture a snapshot and record it into the timeline[cite: 1]
        timeline.record(buildSnapshot(callStack));
    }

    fclose(f); // Close file when execution completes




    // initialize the call stack
    // make the main frame
    // push main frame on the call stack
    // implementation:
    // execute line by line, and according to the keyword perform action

}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // 1. Open the session file for binary writing
    FILE* f = fopen(tdbgPath, "wb");
    if (!f) {
        cerr << "File not Open" << endl;
        return;
    }

    int32_t stepCount = timeline.getStepCount();

    // 2. Prepare and write the initial header placeholder
    TTDBHeader header;
    header.magic[0] = 'T'; header.magic[1] = 'T'; header.magic[2] = 'D'; header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = stepCount;
    header.indexOffset = 0; // Will be updated later after we know where the index starts

    // Write header fields to the beginning of the file
    fwrite(header.magic, 1, 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    // Allocate an array to track the exact file offset of each snapshot step
    int64_t* snapshotOffsets = new int64_t[stepCount];

    // 3. Loop through the timeline, writing each snapshot and recording its file position
    TimelineNode* curr = timeline.begin();
    int32_t index = 0;
    while (curr != nullptr && index < stepCount) {
        // Record the exact starting byte position of this snapshot
        snapshotOffsets[index] = ftell(f);

        // Write the snapshot data structure directly to disk
        fwrite(curr->data, sizeof(Snapshot), 1, f);

        curr = curr->next;
        index++;
    }

    // 4. Capture the file position where the dense index array begins
    int64_t indexArrayOffset = ftell(f);

    // 5. Write the dense index array at the end of the file
    fwrite(snapshotOffsets, sizeof(int64_t), stepCount, f);

    // 6. Seek back to the very beginning of the file (position 0) and update the header
    header.indexOffset = indexArrayOffset;
    fseek(f, 0, SEEK_SET);

    fwrite(header.magic, 1, 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    // Clean up dynamic memory and close the file
    delete[] snapshotOffsets;
    fclose(f);


    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}