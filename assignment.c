#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <stdlib.h>

#define NUM_PROCS 4
#define CACHE_SIZE 4
#define MEM_SIZE 16
#define MSG_BUFFER_SIZE 256
#define MAX_INSTR_NUM 32

typedef unsigned char byte;

typedef enum
{
    MODIFIED,
    EXCLUSIVE,
    SHARED,
    INVALID
} cacheLineState;

typedef enum
{
    EM,
    S,
    U
} directoryEntryState;

typedef enum
{
    READ_REQUEST,  
    WRITE_REQUEST, 
    REPLY_RD,      
    REPLY_WR,      
    REPLY_ID,      
    INV,           
    UPGRADE,       
    WRITEBACK_INV, 
    WRITEBACK_INT, 
    FLUSH,         
    FLUSH_INVACK,  
    EVICT_SHARED,  
    EVICT_MODIFIED 
} transactionType;



typedef struct instruction
{
    byte type;
    byte address;
    byte value;
} instruction;

typedef struct cacheLine
{
    byte address;         
    byte value;           
    cacheLineState state; 
} cacheLine;

typedef struct directoryEntry
{
    byte bitVector; 
                    
    directoryEntryState state;
} directoryEntry;


typedef struct message
{
    transactionType type;
    int sender;                   
    byte address;                 
    byte value;                   
    byte bitVector;               
    int secondReceiver;           
                                  
    directoryEntryState dirState; 
} message;

typedef struct messageBuffer
{
    message queue[MSG_BUFFER_SIZE];
    int head;
    int tail;
    int count; 
} messageBuffer;

typedef struct processorNode
{
    cacheLine cache[CACHE_SIZE];
    byte memory[MEM_SIZE];
    directoryEntry directory[MEM_SIZE];
    instruction instructions[MAX_INSTR_NUM];
    int instructionCount;
} processorNode;

void initializeProcessor(int threadId, processorNode *node, char *dirName);
void sendMessage(int receiver, message msg);                     
void handleCacheReplacement(int sender, cacheLine oldCacheLine); 
void printProcessorState(int processorId, processorNode node);

messageBuffer messageBuffers[NUM_PROCS];
omp_lock_t msgBufferLocks[NUM_PROCS];

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <test_directory>\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *dirName = argv[1];
    omp_set_num_threads(NUM_PROCS);

    for (int i = 0; i < NUM_PROCS; i++)
    {
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
        message msgReply;
        instruction instr;
        int instructionIdx = -1;
        int printProcState = 1;   
        byte waitingForReply = 0; 
                                 
        while (1)
        {

            omp_set_lock(&msgBufferLocks[threadId]);
            while (
                messageBuffers[threadId].count > 0 &&
                messageBuffers[threadId].head != messageBuffers[threadId].tail)
            {
                if (printProcState == 0)
                {
                    printProcState++;
                }
                int head = messageBuffers[threadId].head;
                msg = messageBuffers[threadId].queue[head];
                messageBuffers[threadId].head = (head + 1) % MSG_BUFFER_SIZE;
                messageBuffers[threadId].count--; 
                omp_unset_lock(&msgBufferLocks[threadId]);

#ifdef DEBUG_MSG
                printf("%d - Processor %d msg from: %d, type: %d, address: 0x%02X\n",
                       waitingForReply,threadId, msg.sender, msg.type, msg.address);
#endif /* ifdef DEBUG */

                
                byte procNodeAddr = (msg.address >> 4) &0x0F;
                byte memBlockAddr = msg.address & 0x0F;
                byte cacheIndex = memBlockAddr % CACHE_SIZE;
                byte sharersList;
				
				
                switch (msg.type)
                {
                case READ_REQUEST:
                    
                    if (node.directory[memBlockAddr].state == S)
                    {
                        node.directory[memBlockAddr].bitVector |= (1 << msg.sender);

                        msgReply.type = REPLY_RD;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.value = node.memory[memBlockAddr];
                        msgReply.dirState = S; 
                        sendMessage(msg.sender, msgReply);
                    }
                    else if (node.directory[memBlockAddr].state == EM)
                    {
                        int owner = -1;
                        for (int i = 0; i < NUM_PROCS; i++)
                        {
                            if ((node.directory[memBlockAddr].bitVector & (1 << i)) != 0)
                            {
                                owner = i;
                                break;
                            }
                        }
                        msgReply.type = WRITEBACK_INT;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        
                        msgReply.secondReceiver = msg.sender;
                        sendMessage(owner, msgReply);
                    }
					else
                    {
                        node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                        node.directory[memBlockAddr].state = EM;
						
                        msgReply.type = REPLY_RD;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.value = node.memory[memBlockAddr];
                        msgReply.dirState = EM; 
                        sendMessage(msg.sender, msgReply);
                    }
                    break;

                case REPLY_RD:
                    
                    if (node.cache[cacheIndex].state != INVALID &&
                        node.cache[cacheIndex].address != msg.address)
                    {
                        handleCacheReplacement(threadId, node.cache[cacheIndex]);
                    }

                    node.cache[cacheIndex].address = msg.address;
                    node.cache[cacheIndex].value = msg.value;
                    node.cache[cacheIndex].state = (msg.dirState == S) ? SHARED : EXCLUSIVE; // Use dirState

                    waitingForReply = 0;
                    break;

                case WRITEBACK_INT:
                    
					msgReply.type = FLUSH;
					msgReply.sender = threadId;
					msgReply.address = msg.address;
					msgReply.value = node.cache[cacheIndex].value;
					msgReply.secondReceiver=msg.secondReceiver;
					sendMessage(msg.sender, msgReply);

					if (msg.secondReceiver != msg.sender)
					{
						sendMessage(msg.secondReceiver, msgReply);
					}

					node.cache[cacheIndex].state = SHARED;
                    break;

                case FLUSH:
                    
                    if (threadId == procNodeAddr)
                    {
                        node.memory[memBlockAddr] = msg.value;
                        node.directory[memBlockAddr].state = S;
                        node.directory[memBlockAddr].bitVector = (1 << msg.sender);
						node.directory[memBlockAddr].bitVector |= (1 << msg.secondReceiver);
                    }
                    if(threadId==msg.secondReceiver)
                    {
                        if (node.cache[cacheIndex].state != INVALID &&
                            node.cache[cacheIndex].address != msg.address)
                        {
                            handleCacheReplacement(threadId, node.cache[cacheIndex]);
                        }

                        node.cache[cacheIndex].address = msg.address;
                        node.cache[cacheIndex].value = msg.value;
                        node.cache[cacheIndex].state = SHARED;

                        waitingForReply = 0;
                    }
                    break;

                case UPGRADE:
                    
                    sharersList = node.directory[memBlockAddr].bitVector & ~(1 << msg.sender);

                    msgReply.type = REPLY_ID;
                    msgReply.sender = threadId;
                    msgReply.address = msg.address;
                    msgReply.bitVector = sharersList;
                    sendMessage(msg.sender, msgReply);

                    node.directory[memBlockAddr].state = EM;
                    node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                    break;

                case REPLY_ID:
                    
                    for (int i = 0; i < NUM_PROCS; i++)
                    {
                        if ((msg.bitVector & (1 << i)) != 0)
                        {

                            message invMsg;
                            invMsg.type = INV;
                            invMsg.sender = threadId;
                            invMsg.address = msg.address;
                            sendMessage(i, invMsg);
                        }
                    }
					
					if (node.cache[cacheIndex].state != INVALID &&
                            node.cache[cacheIndex].address != msg.address)
                    {
                        handleCacheReplacement(threadId, node.cache[cacheIndex]);
                    }
					
					node.cache[cacheIndex].address = msg.address;
                    node.cache[cacheIndex].value = msg.value; 
                    if (instructionIdx >= 0 && instructionIdx < node.instructionCount &&
                        node.instructions[instructionIdx].address == msg.address &&
                        node.instructions[instructionIdx].type == 'W')
                    {
                        node.cache[cacheIndex].value = node.instructions[instructionIdx].value;
                    }
                    node.cache[cacheIndex].state = MODIFIED;

                    waitingForReply = 0;
                    break;

                case INV:

					if (node.cache[cacheIndex].address == msg.address && node.cache[cacheIndex].state != INVALID)
					{
						node.cache[cacheIndex].state = INVALID;
					}
                    break;

                case WRITE_REQUEST:
                    
                    if (node.directory[memBlockAddr].state == S)
                    {
                        sharersList = node.directory[memBlockAddr].bitVector & ~(1 << msg.sender);

                        msgReply.type = REPLY_ID;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.bitVector = sharersList;
                        msgReply.value = node.memory[memBlockAddr];
                        sendMessage(msg.sender, msgReply);

                        node.directory[memBlockAddr].state = EM;
                        node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                    }
                    else if (node.directory[memBlockAddr].state == EM)
                    {
                        int owner = -1;
                        for (int i = 0; i < NUM_PROCS; i++)
                        {
                            if ((node.directory[memBlockAddr].bitVector & (1 << i)) != 0)
                            {
                                owner = i;
                                break;
                            }
                        }

                        msgReply.type = WRITEBACK_INV;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        
                        msgReply.secondReceiver = msg.sender;
                        sendMessage(owner, msgReply);
                    }
					else
                    {
                        node.directory[memBlockAddr].bitVector = (1 << msg.sender);
                        node.directory[memBlockAddr].state = EM;

                        msgReply.type = REPLY_WR;
                        msgReply.sender = threadId;
                        msgReply.address = msg.address;
                        msgReply.value = node.memory[memBlockAddr];
                        sendMessage(msg.sender, msgReply);
                    }
                    break;

                case REPLY_WR:
                    if (node.cache[cacheIndex].state != INVALID &&
                        node.cache[cacheIndex].address != msg.address)
                    {
                        handleCacheReplacement(threadId, node.cache[cacheIndex]);
                    }

                    node.cache[cacheIndex].address = msg.address;
                    node.cache[cacheIndex].value = msg.value; 
                    if (instructionIdx >= 0 && instructionIdx < node.instructionCount &&
                        node.instructions[instructionIdx].address == msg.address &&
                        node.instructions[instructionIdx].type == 'W')
                    {
                        node.cache[cacheIndex].value = node.instructions[instructionIdx].value;
                    }
                    node.cache[cacheIndex].state = MODIFIED;

                    waitingForReply = 0;
                    break;

                case WRITEBACK_INV:
                    
					msgReply.type = FLUSH_INVACK;
					msgReply.sender = threadId;
					msgReply.address = msg.address;
					msgReply.value = node.cache[cacheIndex].value;
					msgReply.secondReceiver=msg.secondReceiver;
					sendMessage(msg.sender, msgReply);

					if (msg.secondReceiver != msg.sender)
					{
						sendMessage(msg.secondReceiver, msgReply);
					}

					node.cache[cacheIndex].state = INVALID;
					
					break;

                case FLUSH_INVACK:
                    
					if (threadId == procNodeAddr)
                    {
                        node.memory[memBlockAddr] = msg.value;
                        node.directory[memBlockAddr].state = EM;
                        node.directory[memBlockAddr].bitVector = (1 << msg.secondReceiver);
						
                    }
                    if(threadId == msg.secondReceiver)
                    {
                        if (node.cache[cacheIndex].state != INVALID &&
                            node.cache[cacheIndex].address != msg.address)
                        {
                            handleCacheReplacement(threadId, node.cache[cacheIndex]);
                        }

                        node.cache[cacheIndex].address = msg.address;
                        if (instructionIdx >= 0 && instructionIdx < node.instructionCount &&
                            node.instructions[instructionIdx].address == msg.address &&
                            node.instructions[instructionIdx].type == 'W')
                        {
                            node.cache[cacheIndex].value = node.instructions[instructionIdx].value;
                        }
                        else
                        {
                            node.cache[cacheIndex].value = msg.value;
                        }
                        node.cache[cacheIndex].state = MODIFIED;
                        waitingForReply = 0;
                    }
                    break;

                case EVICT_SHARED:
                    
                    if (threadId == procNodeAddr)
                    {
                        node.directory[memBlockAddr].bitVector &= ~(1 << msg.sender);

                        int numSharers = 0;
                        int lastSharer = -1;
                        for (int i = 0; i < NUM_PROCS; i++)
                        {
                            if ((node.directory[memBlockAddr].bitVector & (1 << i)) != 0)
                            {
                                numSharers++;
                                lastSharer = i;
                            }
                        }

                        if (numSharers == 0)
                        {
                            node.directory[memBlockAddr].state = U;
                        }
                        else if (numSharers == 1)
                        {
							
                            node.directory[memBlockAddr].state = EM;
							if(lastSharer=threadId)
							{
								node.cache[cacheIndex].state=EXCLUSIVE;
							}
							else
							{
								msgReply.type = EVICT_SHARED;
								msgReply.sender = threadId;
								msgReply.address = msg.address;
								sendMessage(lastSharer, msgReply);
							}
                        }
                    }
                    else
                    {
						if (node.cache[cacheIndex].address == msg.address && node.cache[cacheIndex].state == SHARED)
						{
							node.cache[cacheIndex].state = EXCLUSIVE;
						}
                    }
                    break;

                case EVICT_MODIFIED:
                    
					node.memory[memBlockAddr] = msg.value;
					
					node.directory[memBlockAddr].state = U;
					node.directory[memBlockAddr].bitVector = 0;
                    break;
                }
                omp_set_lock(&msgBufferLocks[threadId]);
            }
            omp_unset_lock(&msgBufferLocks[threadId]);
            
            if (waitingForReply > 0)
            {
#ifdef DEBUG
                printf("Processor %d: waiting for reply\n", threadId);
#endif
                continue;
            }
            if (instructionIdx < node.instructionCount - 1)
            {
                instructionIdx++;
            }
            else
            {
                if (printProcState > 0)
                {
                    printProcessorState(threadId, node);
                    printProcState--;
                }
                
                continue;
            }
            instr = node.instructions[instructionIdx];

#ifdef DEBUG_INSTR
            printf("Processor %d: instr type=%c, address=0x%02X, value=%hhu\n",
                   threadId, instr.type, instr.address, instr.value);
#endif

            
            byte procNodeAddr = (instr.address >> 4) & 0x0F;
            byte memBlockAddr = instr.address & 0x0F;
            byte cacheIndex = memBlockAddr % CACHE_SIZE;

            
            if (instr.type == 'R')
            {
                
                int cacheHit = 0;
				if (node.cache[cacheIndex].address == instr.address && node.cache[cacheIndex].state != INVALID)
				{
					cacheHit = 1;
				}
                if (!cacheHit)
                {
                    message readReq;
                    readReq.type = READ_REQUEST;
                    readReq.sender = threadId;
                    readReq.address = instr.address;
                    sendMessage(procNodeAddr, readReq);

                    waitingForReply = 1;
                }
            }
            else
            {
                
                int cacheHit = 0;
				
				if (node.cache[cacheIndex].address == instr.address && node.cache[cacheIndex].state != INVALID)
				{
					cacheHit = 1;
				}

                if (cacheHit)
                {
                    if (node.cache[cacheIndex].state == MODIFIED ||
                        node.cache[cacheIndex].state == EXCLUSIVE)
                    {
                        node.cache[cacheIndex].value = instr.value;
                        node.cache[cacheIndex].state = MODIFIED;
                    }
                    else if (node.cache[cacheIndex].state == SHARED)
                    {
                        message upgradeReq;
                        upgradeReq.type = UPGRADE;
                        upgradeReq.sender = threadId;
                        upgradeReq.address = instr.address;
						upgradeReq.value = instr.value; 
                        sendMessage(procNodeAddr, upgradeReq);

                        waitingForReply = 1;
                    }
                }
                else
                {
                    message writeReq;
                    writeReq.type = WRITE_REQUEST;
                    writeReq.sender = threadId;
                    writeReq.address = instr.address;
                    writeReq.value = instr.value;
                    sendMessage(procNodeAddr, writeReq);

                    waitingForReply = 1;
                }
            }
        }
	}
}

void sendMessage(int receiver, message msg)
{
    
    omp_set_lock(&msgBufferLocks[receiver]);

    int tail = messageBuffers[receiver].tail;
    messageBuffers[receiver].queue[tail] = msg;
    messageBuffers[receiver].tail = (tail + 1) % MSG_BUFFER_SIZE;
    messageBuffers[receiver].count++;

    omp_unset_lock(&msgBufferLocks[receiver]);
}

void handleCacheReplacement(int sender, cacheLine oldCacheLine)
{
    
	if (oldCacheLine.address == 0xFF) return;
    byte procNodeAddr = (oldCacheLine.address >> 4) & 0x0F;
    byte memBlockAddr = oldCacheLine.address & 0x0F;
	
	if (procNodeAddr >= NUM_PROCS) {
        printf("ERROR: Invalid processor ID %d in address 0x%02X\n", 
               procNodeAddr, oldCacheLine.address);
        return;
    }

    if (oldCacheLine.state != INVALID)
    {
        switch (oldCacheLine.state)
        {
        case EXCLUSIVE:
        case SHARED:
            
            {
                message msg;
                msg.type = EVICT_SHARED;
                msg.sender = sender;
                msg.address = oldCacheLine.address;
                msg.value = oldCacheLine.value;
                sendMessage(procNodeAddr, msg);
            }
            break;

        case MODIFIED:
            
            {
                message msg;
                msg.type = EVICT_MODIFIED;
                msg.sender = sender;
                msg.address = oldCacheLine.address;
                msg.value = oldCacheLine.value;
                sendMessage(procNodeAddr, msg);
            }
            break;
        }
	}
}

void initializeProcessor( int threadId, processorNode *node, char *dirName ) {
    for ( int i = 0; i < MEM_SIZE; i++ ) {
        node->memory[ i ] = 20 * threadId + i;  
        node->directory[ i ].bitVector = 0;     
        node->directory[ i ].state = U;         
    }

    for ( int i = 0; i < CACHE_SIZE; i++ ) {
        node->cache[ i ].address = 0xFF;       
        node->cache[ i ].value = 0;
        node->cache[ i ].state = INVALID;       
    }

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

void print_bits(FILE* file, byte byte) {
    for (int i = 7; i >= 0; i--) { 
        fprintf(file, "%d", (byte >> i) & 1); 
    }
}

void printProcessorState(int processorId, processorNode node) {
	
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

    fprintf(file, "-------- Memory State --------\n");
    fprintf(file, "| Index | Address |   Value  |\n");
    fprintf(file, "|----------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %5d   |\n", i, (processorId << 4) + i,
                node.memory[i]);
    }
    fprintf(file, "------------------------------\n\n");

    fprintf(file, "------------ Directory State ---------------\n");
    fprintf(file, "| Index | Address | State |    BitVector   |\n");
    fprintf(file, "|------------------------------------------|\n");
    for (int i = 0; i < MEM_SIZE; i++) {
        fprintf(file, "|  %3d  |  0x%02X   |  %2s   |   0x",
                i, (processorId << 4) + i, dirStateStr[node.directory[i].state]);
		print_bits(file, node.directory[i].bitVector);
		fprintf(file, "   |\n");
    }
    fprintf(file, "--------------------------------------------\n\n");
    
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