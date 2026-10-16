#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <cstdio>

using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;

template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack()
    {
        top = nullptr;
        count = 0;
    }

    void push(const T& val)
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
    }

    T& peek()
    {
        if (isEmpty()) {
            throw runtime_error("Stack Empty");
        }
        return top->data;
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
    }
};

struct Snapshot;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }

    void record(Snapshot* s)
    {
        TimelineNode* new_node = new TimelineNode();
        new_node->next = nullptr;
        new_node->prev = nullptr;
        new_node->data = s;

        if (head == nullptr) {
            head = tail = new_node;
        }
        else {
            tail->next = new_node;
            new_node->prev = tail;
            tail = new_node;
        }
        stepCount++;
    }

    TimelineNode* begin()
    {
        return head;
    }

    int32_t getStepCount()
    {
        return stepCount;
    }
};

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
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
}

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

bool readSourceLine(ifstream& in, string& out)
{
    while (getline(in, out)) {
        if (!out.empty()) return true;
    }
    return false;
}

string firstWord(const string& line)
{
    stringstream ss(line);
    string firstword;
    ss >> firstword;
    if (!firstword.empty()) return firstword;
    return "";
}

string secondWord(const string& line)
{
    stringstream ss(line);
    string firstword, secondword;
    ss >> firstword;
    ss >> secondword;
    return secondword;
}

bool validateProgram(const char* sourcePath)
{
    ifstream fin(sourcePath);
    if (!fin) return false;

    bool inside_func = false;
    bool main_exists = false;
    string line;

    while (readSourceLine(fin, line)) {
        if (firstWord(line) == "func") {
            if (inside_func) {
                return false;
            }
            inside_func = true;

            if (secondWord(line) == "main") {
                main_exists = true;
            }
        }
        else if (firstWord(line) == "func_end") {
            if (!inside_func) {
                return false;
            }
            inside_func = false;
        }
    }

    return (inside_func == false && main_exists == true);
}

int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t initial_pos = ftell(f);
    fwrite(&offsetField, sizeof(int64_t), 1, f);

    int32_t size = text.size();
    fwrite(&size, sizeof(int32_t), 1, f);

    fwrite(text.data(), sizeof(char), size, f);

    return initial_pos;
}

int64_t readResolveRecord(FILE* f, string& outText)
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
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream sf(sourcePath);
    if (!sf.is_open()) return -1;

    FILE* rf = fopen(resolveBinPath, "wb+");
    if (!rf) return -1;

    string line;
    int64_t currentOffset = 0;
    int64_t mainOffset = -1;

    while (readSourceLine(sf, line)) {
        int64_t recordStart = currentOffset;

        fseek(rf, currentOffset, SEEK_SET);

        int64_t fileOffsetPlaceholder = 0;
        fwrite(&fileOffsetPlaceholder, sizeof(int64_t), 1, rf);

        int32_t stringSize = static_cast<int32_t>(line.size());
        fwrite(&stringSize, sizeof(int32_t), 1, rf);

        fwrite(line.c_str(), 1, stringSize, rf);

        currentOffset += sizeof(int64_t) + sizeof(int32_t) + stringSize;

        if (firstWord(line) == "func") {
            if (funcCount < MAX_FUNCS) {
                funcArray[funcCount].funcName = secondWord(line);
                funcArray[funcCount++].byteOffsetInResolveBin = recordStart;
            }
            if (secondWord(line) == "main") {
                mainOffset = recordStart;
            }
        }
        else if (firstWord(line) == "call") {
            if (patchCount < MAX_PATCHES) {
                patches[patchCount].byteOffsetOfOffsetField = recordStart;
                patches[patchCount++].targetFuncName = secondWord(line);
            }
        }
    }

    for (int32_t i = 0; i < patchCount; i++) {
        int64_t targetOffset = -1;

        for (int32_t j = 0; j < funcCount; j++) {
            if (funcArray[j].funcName == patches[i].targetFuncName) {
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetOffset != -1) {
            fseek(rf, patches[i].byteOffsetOfOffsetField, SEEK_SET);
            fwrite(&targetOffset, sizeof(int64_t), 1, rf);
        }
        else {
            cerr << "Error: Unresolved target: " << patches[i].targetFuncName << endl;
            fclose(rf);
            sf.close();
            return -1;
        }
    }

    fclose(rf);
    sf.close();

    if (mainOffset == -1) return -1;

    return mainOffset;
}

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

int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    stringstream ss(line);
    string word;
    int32_t tokenct = 0;

    while (ss >> word && tokenct < maxTokens) {
        if (tokenct == 0) {
            tokens[tokenct].type = KEYWORD;
        }
        else if (tokenct == 1) {
            tokens[tokenct].type = IDENTIFIER;
        }
        else {
            tokens[tokenct].type = PARAM;
        }

        tokens[tokenct].text = word;
        tokenct++;
    }
    return tokenct;
}

Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f) return;

    Stack<Frame> callStack;

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);

    timeline.record(buildSnapshot(callStack));

    fseek(f, mainOffset, SEEK_SET);

    while (true) {
        int64_t storedOffsetField;
        if (fread(&storedOffsetField, sizeof(int64_t), 1, f) != 1) break;

        int32_t stringSize;
        if (fread(&stringSize, sizeof(int32_t), 1, f) != 1) break;

        string rawLine(stringSize, ' ');
        if (fread(&rawLine[0], 1, stringSize, f) != static_cast<size_t>(stringSize)) break;

        Token tokens[MAX_TOKENS];
        int32_t tokenCount = tokenizeLine(rawLine, tokens, MAX_TOKENS);
        if (tokenCount == 0) continue;

        string keyword = tokens[0].text;

        if (keyword == "func_end") {
            if (!callStack.isEmpty()) {
                Frame finishedFrame = callStack.pop();

                if (callStack.isEmpty()) {
                    break;
                }
                else {
                    fseek(f, finishedFrame.returnLine, SEEK_SET);
                }
            }
        }
        else if (keyword == "call") {
            int64_t returnOffset = ftell(f);

            Frame newFrame;
            newFrame.func_name = tokens[1].text;
            newFrame.localCount = 0;
            newFrame.returnLine = static_cast<int32_t>(returnOffset);
            callStack.push(newFrame);

            fseek(f, storedOffsetField, SEEK_SET);
        }
        else if (keyword == "set") {
            if (!callStack.isEmpty() && tokenCount >= 3) {
                Frame& currentFrame = callStack.peek();
                string varName = tokens[1].text;
                int32_t val = stoi(tokens[2].text);

                bool found = false;
                for (int32_t i = 0; i < currentFrame.localCount; i++) {
                    if (currentFrame.locals[i].name == varName) {
                        currentFrame.locals[i].value = val;
                        found = true;
                        break;
                    }
                }
                if (!found && currentFrame.localCount < MAX_VARS_PER_FRAME) {
                    currentFrame.locals[currentFrame.localCount].name = varName;
                    currentFrame.locals[currentFrame.localCount].value = val;
                    currentFrame.localCount++;
                }
            }
        }

        timeline.record(buildSnapshot(callStack));
    }

    fclose(f);
}

void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb");
    if (!f) return;

    int32_t stepct = timeline.getStepCount();

    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = stepct;
    header.indexOffset = 0;

    fwrite(header.magic, 1, 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    int64_t* snapshotOffsets = new int64_t[stepct];

    TimelineNode* curr = timeline.begin();
    int32_t index = 0;
    while (curr != nullptr && index < stepct) {
        snapshotOffsets[index] = ftell(f);
        fwrite(curr->data, sizeof(Snapshot), 1, f);

        curr = curr->next;
        index++;
    }

    int64_t indexArrayOffset = ftell(f);
    fwrite(snapshotOffsets, sizeof(int64_t), stepct, f);

    header.indexOffset = indexArrayOffset;
    fseek(f, 0, SEEK_SET);

    fwrite(header.magic, 1, 4, f);
    fwrite(&header.version, sizeof(int32_t), 1, f);
    fwrite(&header.stepCount, sizeof(int32_t), 1, f);
    fwrite(&header.indexOffset, sizeof(int64_t), 1, f);

    delete[] snapshotOffsets;
    fclose(f);
}

int32_t main()
{
    if (!validateProgram("source.bin")) {
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    if (mainOffset == -1) return 1;

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}