#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

typedef enum { MODIFIED, EXCLUSIVE, SHARED, INVALID } cacheLineState;
typedef enum { EM, S, U } directoryEntryState;
typedef enum { 
    READ_REQUEST, WRITE_REQUEST, REPLY_RD, REPLY_WR, REPLY_ID, INV, UPGRADE,
    WRITEBACK_INV, WRITEBACK_INT, FLUSH, FLUSH_INVACK, EVICT_SHARED, EVICT_MODIFIED
} transactionType;

typedef struct instruction {
    byte type;
    byte address;
    byte value;
} instruction;

typedef struct cacheLine {
    byte address;
    byte value;
    cacheLineState state;
} cacheLine;

typedef struct directoryEntry {
    byte bitVector;
    directoryEntryState state;
} directoryEntry;

typedef struct message {
    transactionType type;
    int sender;
    byte address;
    byte value;
    byte bitVector;
    int secondReceiver;
    directoryEntryState dirState;
} message;

typedef struct messageBuffer {
    message queue[MSG_BUFFER_SIZE];
    int head;
    int tail;
    int count;
} messageBuffer;

typedef struct processorNode {
    cacheLine cache[CACHE_SIZE];
    byte memory[MEM_SIZE];
    directoryEntry directory[MEM_SIZE];
    instruction instructions[MAX_INSTR_NUM];
    int instructionCount;
} processorNode;

messageBuffer messageBuffers[NUM_PROCS];
omp_lock_t msgBufferLocks[NUM_PROCS];

void initializeProcessor(int threadId, processorNode *node, char *dirName);
void sendMessage(int receiver, message msg);
void handleCacheReplacement(int sender, cacheLine oldCacheLine);
void printProcessorState(int processorId, processorNode node);

int numberOfSetBits(byte bitVector) {
    int count = 0;
    for (int i = 0; i < 8; i++) {
        if (bitVector & (1 << i)) {
            count++;
        }
    }
    return count;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test_directory>\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];

    omp_set_num_threads(NUM_PROCS);

    for (int i = 0; i < NUM_PROCS; i++) {
        messageBuffers[i].count = 0;
        messageBuffers[i].head = 0;
        messageBuffers[i].tail = 0;
        omp_init_lock(&msgBufferLocks[i]);
    }

    processorNode node;

    #pragma omp parallel private(node)
    {
        int threadId = omp_get_thread_num();
        initializeProcessor(threadId, &node, dirName);
        #pragma omp barrier

        message msg;
        instruction instr;
        int instructionIdx = -1;
        int printProcState = 1;
        byte waitingForReply = 0;

        while (1) {
            int processedMessages = 0;

            omp_set_lock(&msgBufferLocks[threadId]);
            while (messageBuffers[threadId].count > 0 && messageBuffers[threadId].head != messageBuffers[threadId].tail) {
                if (printProcState == 0) printProcState++;
                int head = messageBuffers[threadId].head;
                msg = messageBuffers[threadId].queue[head];
                messageBuffers[threadId].head = (head + 1) % MSG_BUFFER_SIZE;
                messageBuffers[threadId].count--;
                processedMessages++;
                omp_unset_lock(&msgBufferLocks[threadId]);
                
                byte procNodeAddr = (msg.address >> 4) & 0x0F;
                byte memBlockAddr = msg.address & 0x0F;
                int cacheIndex = memBlockAddr % CACHE_SIZE;

                switch (msg.type) {
                    case READ_REQUEST: {
                        directoryEntry *entry = &node.directory[memBlockAddr];
                        if (entry->state == U) {
                            entry->state = EM;
                            entry->bitVector = (1 << msg.sender);
                            message reply = {REPLY_RD, threadId, msg.address, node.memory[memBlockAddr], 0, -1, EM};
                            sendMessage(msg.sender, reply);
                        } else if (entry->state == S) {
                            entry->bitVector |= (1 << msg.sender);
                            message reply = {REPLY_RD, threadId, msg.address, node.memory[memBlockAddr], 0, -1, S};
                            sendMessage(msg.sender, reply);
                        } else if (entry->state == EM) {
                            int owner = 0;
                            for (; owner < NUM_PROCS; owner++)
                                if (entry->bitVector & (1 << owner)) break;
                            entry->bitVector |= (1 << msg.sender);
                            message wb_int = {WRITEBACK_INT, threadId, msg.address, 0, 0, msg.sender, EM};
                            sendMessage(owner, wb_int);
                        }
                        break;
                    }
                    case REPLY_RD: {
                        cacheLine *line = &node.cache[cacheIndex];
                        if (line->state != INVALID)
                            handleCacheReplacement(threadId, *line);
                        line->address = msg.address;
                        line->value = msg.value;
                        line->state = msg.dirState == EM ? EXCLUSIVE : SHARED;
                        waitingForReply--;
                        break;
                    }
                    case WRITEBACK_INT: {
                        cacheLine *line = &node.cache[cacheIndex];
                        if (line->state == MODIFIED || line->state == EXCLUSIVE) {
                            message flush = {FLUSH, threadId, msg.address, line->value, 0, msg.secondReceiver, EM};
                            sendMessage(msg.sender, flush);
                            if (msg.secondReceiver != msg.sender)
                                sendMessage(msg.secondReceiver, flush);
                            line->state = SHARED;
                        } else if (line->state == SHARED) {
                            message flush = {FLUSH, threadId, msg.address, line->value, 0, msg.secondReceiver, S};
                            sendMessage(msg.sender, flush);
                            if (msg.secondReceiver != msg.sender)
                                sendMessage(msg.secondReceiver, flush);
                        } else if (line->state == INVALID) {
                            message direct_reply = {REPLY_RD, threadId, msg.address, 0, 0, -1, U};
                            sendMessage(msg.secondReceiver, direct_reply);
                        }
                        break;
                    }
                    case FLUSH: {
                        byte isHome = (procNodeAddr == threadId);
                        if (isHome) {
                            node.memory[memBlockAddr] = msg.value;
                            directoryEntry *entry = &node.directory[memBlockAddr];
                            entry->bitVector |= (1 << msg.secondReceiver);
                            if (entry->state == EM)
                                entry->state = S;

                            message reply_RD = {REPLY_RD, threadId, msg.address, node.memory[memBlockAddr], 0, -1, entry->state};
                            sendMessage(msg.secondReceiver, reply_RD);
                        } else {
                            cacheLine *line = &node.cache[cacheIndex];
                            if (line->state != INVALID)
                                handleCacheReplacement(threadId, *line);
                            line->address = msg.address;
                            line->value = msg.value;
                            line->state = SHARED;
                        }
                        break;
                    }
                    case UPGRADE: {
                        directoryEntry *entry = &node.directory[memBlockAddr];
                        byte sharers = entry->bitVector & ~(1 << msg.sender);
                        entry->state = EM;
                        entry->bitVector = (1 << msg.sender);
                        message reply_id = {REPLY_ID, threadId, msg.address, msg.value, sharers, -1, EM};
                        sendMessage(msg.sender, reply_id);
                        break;
                    }
                    case REPLY_ID: {
                        for (int i = 0; i < NUM_PROCS; i++)
                            if (msg.bitVector & (1 << i)) {
                                message inv = {INV, threadId, msg.address, 0, 0, -1, U};
                                sendMessage(i, inv);
                            }
                        cacheLine *line = &node.cache[cacheIndex];
                        if (line->address == msg.address) {
                            line->value = msg.value;
                            line->state = MODIFIED;
                        } else {
                            if (line->state != INVALID)
                                handleCacheReplacement(threadId, *line);
                            line->address = msg.address;
                            line->value = msg.value;
                            line->state = MODIFIED;
                        }
                        waitingForReply--;
                        break;
                    }
                    case INV: {
                        cacheLine *line = &node.cache[cacheIndex];
                        if (line->address == msg.address)
                            line->state = INVALID;
                        break;
                    }
                    case WRITE_REQUEST: {
                        directoryEntry *entry = &node.directory[memBlockAddr];
                        if (entry->state == U) {
                            entry->state = EM;
                            entry->bitVector = (1 << msg.sender);
                            message reply_wr = {REPLY_WR, threadId, msg.address, msg.value, 0, -1, EM};
                            sendMessage(msg.sender, reply_wr);
                        } else if (entry->state == S) {
                            byte sharers = entry->bitVector & ~(1 << msg.sender);
                            entry->state = EM;
                            entry->bitVector = (1 << msg.sender);
                            message reply_id = {REPLY_ID, threadId, msg.address, msg.value, sharers, -1, EM};
                            sendMessage(msg.sender, reply_id);
                        } else if (entry->state == EM) {
                            int owner = 0;
                            for (; owner < NUM_PROCS; owner++)
                                if (entry->bitVector & (1 << owner)) 
                                    break;
                            message wb_inv = {WRITEBACK_INV, threadId, msg.address, msg.value, 0, msg.sender, EM};
                            sendMessage(owner, wb_inv);
                        }
                        break;
                    }
                    case REPLY_WR: {
                        cacheLine *line = &node.cache[cacheIndex];
                        if (line->state != INVALID)
                            handleCacheReplacement(threadId, *line);
                        line->address = msg.address;
                        line->value = msg.value;
                        line->state = MODIFIED;
                        waitingForReply--;
                        break;
                    }
                    case WRITEBACK_INV: {
                        cacheLine *line = &node.cache[cacheIndex];
                        message flush_invack = {FLUSH_INVACK, threadId, msg.address, msg.value, 0, msg.secondReceiver, EM};
                        sendMessage(msg.sender, flush_invack);
                        if (msg.sender != msg.secondReceiver)
                            sendMessage(msg.secondReceiver, flush_invack);
                        line->state = INVALID;
                        break;
                    }
                    case FLUSH_INVACK: {
                        if (threadId == procNodeAddr) {
                            node.memory[memBlockAddr] = msg.value;
                            directoryEntry *entry = &node.directory[memBlockAddr];
                            entry->bitVector = (1 << msg.secondReceiver);
                            entry->state = EM;
                            message reply_wr = {REPLY_WR, threadId, msg.address, msg.value, 0, -1, EM};
                            sendMessage(msg.secondReceiver, reply_wr);
                        } else {
                            cacheLine *line = &node.cache[cacheIndex];
                            if (line->state != INVALID)
                                handleCacheReplacement(threadId, *line);
                            line->address = msg.address;
                            line->value = msg.value;
                            line->state = MODIFIED;
                        }
                        break;
                    }
                    case EVICT_SHARED: {
                        directoryEntry *entry = &node.directory[memBlockAddr];
                        entry->bitVector &= ~(1 << msg.sender);
                        if (entry->bitVector == 0)
                            entry->state = U;
                        else if (numberOfSetBits(entry->bitVector) == 1) {
                            entry->state = EM;
                            int owner = 0;
                            for (; owner < NUM_PROCS; owner++)
                                if (entry->bitVector & (1 << owner)) break;
                            message evict_shared = {EVICT_SHARED, threadId, msg.address, 0, 0, -1, EM};
                            sendMessage(owner, evict_shared);
                        }
                        break;
                    }
                    case EVICT_MODIFIED: {
                        node.memory[memBlockAddr] = msg.value;
                        directoryEntry *entry = &node.directory[memBlockAddr];
                        entry->bitVector &= ~(1 << msg.sender);
                        entry->state = U;
                        break;
                    }
                }

                omp_set_lock(&msgBufferLocks[threadId]);
            }

            omp_unset_lock(&msgBufferLocks[threadId]);

            if (processedMessages > 0) {
                continue;
            }

            if (waitingForReply) continue;

            if (instructionIdx < node.instructionCount - 1) {
                instructionIdx++;
            } else {
                if (printProcState > 0) {
                    printProcessorState(threadId, node);
                    printProcState--;
                }
                continue;
            }
            instr = node.instructions[instructionIdx];

            #ifdef DEBUG_INSTR
            printf( "Processor %d: instr type=%c, address=0x%02X, value=%hhu\n",
                    threadId, instr.type, instr.address, instr.value );
            #endif

            byte procNodeAddr = instr.address >> 4;
            byte memBlockAddr = instr.address & 0x0F;
            int cacheIndex = memBlockAddr % CACHE_SIZE;

            if (instr.type == 'R') {
                cacheLine *line = &node.cache[cacheIndex];
                if (line->address != instr.address || line->state == INVALID) {
                    message read_req = {READ_REQUEST, threadId, instr.address, 0, 0, -1, U};
                    sendMessage(procNodeAddr, read_req);
                    waitingForReply++;
                }
            } else {
                cacheLine *line = &node.cache[cacheIndex];
                if (line->address == instr.address && line->state != INVALID) {
                    if (line->state == SHARED) {
                        message upgrade_msg = {UPGRADE, threadId, instr.address, instr.value, 0, -1, U};
                        sendMessage(procNodeAddr, upgrade_msg);
                        waitingForReply++;
                    } else {
                        line->value = instr.value;
                        line->state = MODIFIED;
                    }
                } else {
                    message write_req = {WRITE_REQUEST, threadId, instr.address, instr.value, 0, -1, U};
                    sendMessage(procNodeAddr, write_req);
                    waitingForReply++;
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

void sendMessage(int receiver, message msg) {
    if (receiver == omp_get_thread_num()) {
        messageBuffer *buffer = &messageBuffers[receiver];
        if (buffer->count < MSG_BUFFER_SIZE) {
            buffer->queue[buffer->tail] = msg;
            buffer->tail = (buffer->tail + 1) % MSG_BUFFER_SIZE;
            buffer->count++;
        }
    } else {
        omp_set_lock(&msgBufferLocks[receiver]);
        messageBuffer *buffer = &messageBuffers[receiver];
        if (buffer->count < MSG_BUFFER_SIZE) {
            buffer->queue[buffer->tail] = msg;
            buffer->tail = (buffer->tail + 1) % MSG_BUFFER_SIZE;
            buffer->count++;
        }
        omp_unset_lock(&msgBufferLocks[receiver]);
    }
}

void handleCacheReplacement(int sender, cacheLine oldCacheLine) {
    byte memBlockAddr = oldCacheLine.address & 0x0F;
    byte homeNode = (oldCacheLine.address >> 4) & 0x0F;
    message msg;

    switch (oldCacheLine.state) {
        case SHARED:
        case EXCLUSIVE:
            msg.type = EVICT_SHARED;
            msg.sender = sender;
            msg.address = oldCacheLine.address;
            sendMessage(homeNode, msg);
            break;
        case MODIFIED:
            msg.type = EVICT_MODIFIED;
            msg.sender = sender;
            msg.address = oldCacheLine.address;
            msg.value = oldCacheLine.value;
            sendMessage(homeNode, msg);
            break;
        default:
            break;
    }
}

void initializeProcessor( int threadId, processorNode *node, char *dirName ) {
    // IMPORTANT: DO NOT MODIFY
    for ( int i = 0; i < MEM_SIZE; i++ ) {
        node->memory[ i ] = 20 * threadId + i;  // some initial value to mem block
        node->directory[ i ].bitVector = 0;     // no cache has this block at start
        node->directory[ i ].state = U;         // this block is in Unowned state
    }

    for ( int i = 0; i < CACHE_SIZE; i++ ) {
        node->cache[ i ].address = 0xFF;        // this address is invalid as we can
                                                // have a maximum of 8 nodes in the 
                                                // current implementation
        node->cache[ i ].value = 0;
        node->cache[ i ].state = INVALID;       // all cache lines are invalid
    }

    // read and parse instructions from core_<threadId>.txt
    char filename[ 128 ];
    snprintf(filename, sizeof(filename), "tests/%s/core_%d.txt", dirName, threadId);
    FILE *file = fopen( filename, "r" );
    if ( !file ) {
        fprintf( stderr, "Error: count not open file %s\n", filename );
        exit( EXIT_FAILURE );
    }

    char line[ 20 ];
    node->instructionCount = 0;
    while ( fgets( line, sizeof( line ), file ) &&
            node->instructionCount < MAX_INSTR_NUM ) {
        if ( line[ 0 ] == 'R' && line[ 1 ] == 'D' ) {
            sscanf( line, "RD %hhx",
                    &node->instructions[ node->instructionCount ].address );
            node->instructions[ node->instructionCount ].type = 'R';
            node->instructions[ node->instructionCount ].value = 0;
        } else if ( line[ 0 ] == 'W' && line[ 1 ] == 'R' ) {
            sscanf( line, "WR %hhx %hhu",
                    &node->instructions[ node->instructionCount ].address,
                    &node->instructions[ node->instructionCount ].value );
            node->instructions[ node->instructionCount ].type = 'W';
        }
        node->instructionCount++;
    }

    fclose( file );
    printf( "Processor %d initialized\n", threadId );
}

void printProcessorState(int processorId, processorNode node) {
    // IMPORTANT: DO NOT MODIFY
    static const char *cacheStateStr[] = { "MODIFIED", "EXCLUSIVE", "SHARED",
                                           "INVALID" };
    static const char *dirStateStr[] = { "EM", "S", "U" };

    char filename[32];
    snprintf(filename, sizeof(filename), "core_%d_output.txt", processorId);

    FILE *file = fopen(filename, "w");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        return;
    }

    fprintf(file, "=======================================\n");
    fprintf(file, " Processor Node: %d\n", processorId);
    fprintf(file, "=======================================\n\n");

    // Print memory state
    fprintf(file, "-------- Memory State --------\n");
    fprintf(file, "| Index | Address |   Value  |\n");
    fprintf(file, "|----------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i,
                node.memory[i]);
    }
    fprintf(file, "------------------------------\n\n");

    // Print directory state
    fprintf(file, "------------ Directory State ---------------\n");
    fprintf(file, "| Index | Address | State |    BitVector   |\n");
    fprintf(file, "|------------------------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x%08B   |\n",
                i, (processorId << 4) + i, dirStateStr[node.directory[i].state],
                node.directory[i].bitVector);
    }
    fprintf(file, "--------------------------------------------\n\n");
    
    // Print cache state
    fprintf(file, "------------ Cache State ----------------\n");
    fprintf(file, "| Index | Address | Value |    State    |\n");
    fprintf(file, "|---------------------------------------|\n");
    for (int i = 0; i < CACHE_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %3d  |  %8s \t|\n",
               i, node.cache[i].address, node.cache[i].value,
               cacheStateStr[node.cache[i].state]);
    }
    fprintf(file, "----------------------------------------\n\n");

    fclose(file);
}