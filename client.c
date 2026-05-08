#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <cjson/cJSON.h>
#include <dirent.h>
#include <openssl/evp.h>
#include <errno.h>

void sendStuff(char *buffer, int sd, struct sockaddr_in server_address);
void makeSocket(int *sd, char *argv[], struct sockaddr_in *server_address);
FILE *openFile(const char* fileName);

//lab6 functions
char* openDirectory(const char* directory, char* chunkDir);
cJSON* hashFileAndSave(const char* filePath, const char* fileName, const char* chunkDir, int p2pPort);
void binaryToHex(unsigned char* hash, unsigned int length, char* output);

//lab4 functions
char* serializeJson(cJSON* jsonObj, char* buffer);

//lab7 functions
void receiveStuffAndPrintData(int sd, int p2pSD, const char *directory);
void requestChunksFromPeer(struct sockaddr_in peerAddr, int p2pSD, cJSON *fileInfo, const char *directory);
void serveChunkRequest(int p2pSD, const char *directory);

//chunk size 
#define CHUNK_SIZE (500 * 1024)
#define MAX_FRAG_PAYLOAD 1400 

int main(int argc, char *argv[]){
    int sd;
    struct sockaddr_in server_address;
    struct sockaddr_in p2pAddr;
    char buffer[65536];
    int p2pPort;
    int p2pSD;

    DIR* folder;
    struct dirent* entry;

    char* fileName;
    char filePath[PATH_MAX];
    cJSON* jsonObj = cJSON_CreateObject();

    if(argc < 4){
        printf("Usage: <IPaddr> <portNumber> <Directory Name>\n");
        exit(1);
    }

    //make and bind to port for client p2p
    p2pPort = atoi(argv[2]) + 1;
    p2pSD = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&p2pAddr, 0, sizeof(p2pAddr));
    p2pAddr.sin_family = AF_INET;
    p2pAddr.sin_port = htons(p2pPort);
    p2pAddr.sin_addr.s_addr = INADDR_ANY;
    bind(p2pSD, (struct sockaddr *)&p2pAddr, sizeof(p2pAddr));
    int rcvbuf = 4 * 1024 * 1024; // 4MB
    setsockopt(p2pSD, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    makeSocket(&sd, argv, &server_address);

    char *directory = argv[3];

    folder = opendir(directory);
    if(folder == NULL){
        printf("Unable to read directory %s\n", directory);
        exit(1);
    }

    //create directory for the chunks to be saved in
    char chunkDir[PATH_MAX];
    snprintf(chunkDir, sizeof(chunkDir), "%s/CHUNKS", directory);
    if(mkdir(chunkDir, 0755) != 0 && errno != EEXIST){
        perror("mkdir failed");
    }

    //scan directory, hash each file and send registration to server
    while((entry = readdir(folder)) != NULL){

        if (strcmp(entry->d_name, ".") == 0 || 
            strcmp(entry->d_name, "..") == 0 || 
            strcmp(entry->d_name, "CHUNKS") == 0) continue;
        
        fileName = entry->d_name;

        snprintf(filePath, sizeof(filePath), "%s/%s", directory, fileName);
        jsonObj = hashFileAndSave(filePath, fileName, chunkDir, p2pPort);
        sendStuff(serializeJson(jsonObj, buffer), sd, server_address);
        cJSON_Delete(jsonObj);
    }

    closedir(folder);
    printf("Done sending data\n");

    //use select() so incoming requests are served
    //while waiting for user input
    for(;;){
        printf("\nSelect an option:\n1. View Available Files\n2. Download a file\n3. Exit\n> ");
        fflush(stdout);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(p2pSD, &readfds);
        int maxfd = sd;
        if(p2pSD > maxfd) maxfd = p2pSD;
        if(STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if(ready < 0){
            perror("select error");
            exit(1);
        }

        //if data arrived on the socket before user typed, serve it 
        if(FD_ISSET(p2pSD, &readfds)){
            serveChunkRequest(p2pSD, directory);
        }

        //handle user menu input
        if(FD_ISSET(STDIN_FILENO, &readfds)){
            int choice;
            scanf("%d", &choice);

            if(choice == 1){
                //send query to server
                cJSON *query = cJSON_CreateObject();
                cJSON_AddStringToObject(query, "requestType", "query");
                char *serializedQuery = cJSON_PrintUnformatted(query);
                int sent = sendto(sd, serializedQuery, strlen(serializedQuery), 0,
                                  (struct sockaddr *)&server_address, sizeof(server_address));
                if(sent < 0){
                    perror("sendto query failed");
                    exit(1);
                }
                cJSON_Delete(query);
                free(serializedQuery);

                //receive and display the query response
                receiveStuffAndPrintData(sd, p2pSD, directory);
            }
            else if(choice == 2){
                printf("Please use option 1 to view files first, then choose a file to download.\n");
            }
            else if(choice == 3){
                printf("Exiting\n");
                close(sd);
                return 0;
            }
            else{
                printf("Invalid choice\n");
            }
        }
    }

    return 0;
}


//this function takes the filepath for every file in the directory, hashes the full file and each 500KB chunk,
//saves each chunk as its own file in the CHUNKS folder, and returns a JSON object with all file metadata
cJSON* hashFileAndSave(const char* filePath, const char* fileName, const char* chunkDir, int p2pPort){

    FILE* fptr = openFile(filePath);
    if(!fptr){
        perror("openFile error");
        exit(1);
    }

    //get the size of the file and store it
    fseek(fptr, 0, SEEK_END);
    long fileSize = ftell(fptr);
    rewind(fptr);

    unsigned char buffer[CHUNK_SIZE];
    size_t bytesRead; 

    //json objects used to store the info for the file and hashed chunks
    cJSON *jsonObject = cJSON_CreateObject();
    cJSON *chunkArray = cJSON_CreateArray();
    int numOfChunks = 0;

    //initialize the context for the full file hash
    EVP_MD_CTX *fileContext = EVP_MD_CTX_new();
    if(EVP_DigestInit_ex(fileContext, EVP_sha256(), NULL) != 1){
        EVP_MD_CTX_free(fileContext);
        perror("EVP_DigestInit full file");
        exit(1);
    }

    //iterate through the file 500KB at a time, hash each chunk, store the chunk data as a file,
    //and also incrementally update the fullfile hash
    while((bytesRead = fread(buffer, 1, CHUNK_SIZE, fptr)) > 0){
        EVP_MD_CTX *chunkContext = EVP_MD_CTX_new();

        //initialize chunk context
        if(EVP_DigestInit_ex(chunkContext, EVP_sha256(), NULL) != 1){
            EVP_MD_CTX_free(chunkContext);
            perror("EVP_DigestInit chunk");
            exit(1);
        }

        if(EVP_DigestUpdate(chunkContext, buffer, bytesRead) != 1){
            EVP_MD_CTX_free(chunkContext);
            perror("EVP_DigestUpdate chunk");
            exit(1);
        }

        //incrementally update full file hash
        if(EVP_DigestUpdate(fileContext, buffer, bytesRead) != 1){
            EVP_MD_CTX_free(fileContext);
            perror("EVP_DigestUpdate full file");
            exit(1);
        }

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen;

        if(EVP_DigestFinal_ex(chunkContext, hash, &hashLen) != 1){
            EVP_MD_CTX_free(chunkContext);
            perror("EVP_DigestFinal chunk");
            exit(1);
        }

        EVP_MD_CTX_free(chunkContext);

        char hashHex[65];
        binaryToHex(hash, hashLen, hashHex);

        //add chunk info to json array
        cJSON *chunkObj = cJSON_CreateObject();
        cJSON_AddStringToObject(chunkObj, "chunkName", hashHex);
        cJSON_AddNumberToObject(chunkObj, "chunkSize", (double)bytesRead);
        cJSON_AddItemToArray(chunkArray, chunkObj);

      
        char chunkPath[PATH_MAX];
        snprintf(chunkPath, sizeof(chunkPath), "%s/%s", chunkDir, hashHex);

        FILE* chunkFptr = fopen(chunkPath, "wb");
        if (!chunkFptr) {
            perror("fopen chunk failed");
            exit(1);
        }

        fwrite(buffer, 1, bytesRead, chunkFptr); //write raw chunk data so it can be served later
        fclose(chunkFptr);
        numOfChunks++;
    }

    //finalize the fullfile hash
    unsigned char fileHash[EVP_MAX_MD_SIZE];
    unsigned int fileHashLen;
    if(EVP_DigestFinal_ex(fileContext, fileHash, &fileHashLen) != 1){
        EVP_MD_CTX_free(fileContext);
        perror("EVP_DigestFinal full file");
        exit(1);
    }

    EVP_MD_CTX_free(fileContext);

    char fileHashHex[65];
    binaryToHex(fileHash, fileHashLen, fileHashHex);

    //build the upload JSON object
    cJSON_AddStringToObject(jsonObject, "requestType", "upload");
    cJSON_AddStringToObject(jsonObject, "filename", fileName);
    cJSON_AddNumberToObject(jsonObject, "fileSize", (double)fileSize);
    cJSON_AddNumberToObject(jsonObject, "numberOfChunks", numOfChunks);
    cJSON_AddItemToObject(jsonObject, "chunk_hashes", chunkArray);
    cJSON_AddStringToObject(jsonObject, "fullFileHash", fileHashHex);
    cJSON_AddNumberToObject(jsonObject, "Port", p2pPort);

    fclose(fptr);
    return jsonObject;
}


char* serializeJson(cJSON* jsonObj, char* buffer){
    char* serializedObject = cJSON_PrintUnformatted(jsonObj);
    strcpy(buffer, serializedObject);
    free(serializedObject);
    return buffer;
}


void makeSocket(int *sd, char* argv[], struct sockaddr_in *server_address){
    
    struct sockaddr_in inaddr; //temp value for validation checking 
    int portNumber;
    char serverIP[50];

    //checks for valid ip address
    if(!inet_pton(AF_INET, argv[1], &inaddr)){
        printf("error, bad ip address\n");
        exit(1);
    }

    //create socket
    *sd = socket(AF_INET, SOCK_DGRAM, 0);
    if(*sd == -1){
        perror("socket");
        exit(1);
    }

    int reuse = 1;
    setsockopt(*sd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    portNumber = strtol(argv[2], NULL, 10);

    //validate port number
    if((portNumber > 65535) || (portNumber < 0)){
        printf("You entered an invalid socket number\n");
        exit(1);
    }

   
    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(0); //0 = let OS pick an available port
    localAddr.sin_addr.s_addr = INADDR_ANY;
    if(bind(*sd, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0){
        perror("bind");
        exit(1);
    }

    //copy the ip address from cmd line 
    strcpy(serverIP, argv[1]); 
    //fill in the server address data structure
    server_address->sin_family = AF_INET;
    server_address->sin_port = htons(portNumber);
    server_address->sin_addr.s_addr = inet_addr(serverIP);
}


//gets the name of file from user, opens the file, and returns the file pointer
FILE * openFile(const char* fileName){
    FILE * fptr = NULL;

    fptr = fopen(fileName, "rb");
    if(fptr == NULL){
        printf("Error opening file %s\n", fileName);
    }
    return fptr;
}


//converts binary hash into hex string for json output and chunk filenames
void binaryToHex(unsigned char* hash, unsigned int length, char* output){
    for(unsigned int i = 0; i < length; i++){
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[length * 2] = '\0';
}


void sendStuff(char *buffer, int sd, struct sockaddr_in server_address){
    if(!buffer){
        printf("null buffer, skipping send\n");
        return;
    }
    printf("%s\n", buffer);
    int sent = sendto(sd, buffer, strlen(buffer), 0,
                      (struct sockaddr *)&server_address, sizeof(server_address));
    if(sent < 0){
        perror("sendto failed");
        exit(1);
    }   
}


//receive the queryResponse from the server, display the file table, and let user pick a file to download
void receiveStuffAndPrintData(int sd, int p2pSD, const char *directory){
    char buffer[65536];
    struct sockaddr_in fromAddress;
    socklen_t fromLength;
    int rc;
    struct timeval timeout;

    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    for(;;){
        fromLength = sizeof(fromAddress);

        rc = recvfrom(sd, buffer, sizeof(buffer) - 1, 0,
                      (struct sockaddr *)&fromAddress, &fromLength);
        if(rc < 0){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                printf("No response received (timeout)\n");
                break;
            }
            perror("recvfrom failed");
            exit(1);
        }
        buffer[rc] = '\0';

       
        cJSON *peeked = cJSON_Parse(buffer);
        if(peeked){
            cJSON *rtItem = cJSON_GetObjectItem(peeked, "requestType");
            if(rtItem && cJSON_IsString(rtItem) && strcmp(rtItem->valuestring, "getChunk") == 0){
                cJSON_Delete(peeked);
                //reparse inside serveChunkRequest by pushing buffer back 
                //instead, handle it inline here
                cJSON *chunkNameItem = NULL;
                cJSON *req = cJSON_Parse(buffer);
                if(req){
                    chunkNameItem = cJSON_GetObjectItem(req, "chunkName");
                    if(chunkNameItem && cJSON_IsString(chunkNameItem)){
                        char chunkPath[PATH_MAX];
                        snprintf(chunkPath, sizeof(chunkPath), "%s/CHUNKS/%s",
                                 directory, chunkNameItem->valuestring);
                        FILE *chunkFile = fopen(chunkPath, "rb");
                        if(chunkFile){
                            unsigned char chunkData[CHUNK_SIZE + 1024];
                            size_t bytesRead = fread(chunkData, 1, sizeof(chunkData), chunkFile);
                            fclose(chunkFile);
                            sendto(sd, chunkData, bytesRead, 0,
                                   (struct sockaddr *)&fromAddress, sizeof(fromAddress));
                            printf("Served chunk to %s:%d\n",
                                   inet_ntoa(fromAddress.sin_addr), ntohs(fromAddress.sin_port));
                        }
                    }
                    cJSON_Delete(req);
                }
                continue; //keep waiting for the queryResponse
            }
            cJSON_Delete(peeked);
        }

        //parse the query response
        cJSON *rootObj = cJSON_Parse(buffer);
        if(!rootObj){
            printf("Invalid JSON received\n");
            break;
        }

        cJSON *requestTypeItem = cJSON_GetObjectItem(rootObj, "requestType");
        if(!cJSON_IsString(requestTypeItem) || strcmp(requestTypeItem->valuestring, "queryResponse") != 0){
            cJSON_Delete(rootObj);
            continue;
        }

        cJSON *filesItem = cJSON_GetObjectItem(rootObj, "files");
        int size = cJSON_GetArraySize(filesItem);

        printf("\nFiles Available Across Peers:\n");
        printf("----------------------------------------------------------------------\n");
        printf("%-6s | %-25s | %-12s | %s\n", "Choice", "File Name", "Size (bytes)", "Full Hash");
        printf("----------------------------------------------------------------------\n");

        for(int i = 0; i < size; i++){
            cJSON *fileInfo = cJSON_GetArrayItem(filesItem, i);
            if(cJSON_IsObject(fileInfo)){
                cJSON *fileNameItem  = cJSON_GetObjectItem(fileInfo, "filename");
                cJSON *hashItem      = cJSON_GetObjectItem(fileInfo, "fullFileHash");
                cJSON *fileSizeItem  = cJSON_GetObjectItem(fileInfo, "fileSize");
                printf("%-6d | %-25s | %-12ld | %s\n",
                    i + 1,
                    fileNameItem->valuestring,
                    (long)fileSizeItem->valuedouble,
                    hashItem->valuestring);
            }
        }
        printf("----------------------------------------------------------------------\n");

        int choice = 0;
        printf("Enter the number of the file you wish to download (0 to cancel): ");
        fflush(stdout);
        scanf("%d", &choice);

        if(choice == 0){
            cJSON_Delete(rootObj);
            break;
        }

        if(choice < 1 || choice > size){
            printf("Invalid choice\n");
            cJSON_Delete(rootObj);
            break;
        }

        cJSON *fileInfo = cJSON_GetArrayItem(filesItem, choice - 1);
        cJSON *IPInfoItem = cJSON_GetObjectItem(fileInfo, "IPInfo");

        //get the first element to extract IP and Port
        cJSON *firstPeer = cJSON_GetArrayItem(IPInfoItem, 0);
        cJSON *ipItem   = cJSON_GetObjectItem(firstPeer, "IP");
        cJSON *portItem = cJSON_GetObjectItem(firstPeer, "Port");

        struct sockaddr_in peerAddr;
        memset(&peerAddr, 0, sizeof(peerAddr));
        peerAddr.sin_family = AF_INET;
        peerAddr.sin_port = htons((int)portItem->valuedouble);
        inet_pton(AF_INET, ipItem->valuestring, &(peerAddr.sin_addr));

        printf("Connecting to peer %s:%d\n", ipItem->valuestring, (int)portItem->valuedouble);

        requestChunksFromPeer(peerAddr, p2pSD, fileInfo, directory);

        cJSON_Delete(rootObj);
        break;
    }

    //clear the receive timeout after we are done
    struct timeval noTimeout = {0, 0};
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &noTimeout, sizeof(noTimeout));
}


//request each chunk from a peer over UDP, reassembling fragments before validating hash
void requestChunksFromPeer(struct sockaddr_in peerAddr, int p2pSD, cJSON *fileInfo, const char *directory){

    cJSON *chunkHashesItem  = cJSON_GetObjectItem(fileInfo, "chunk_hashes");
    cJSON *filenameItem     = cJSON_GetObjectItem(fileInfo, "filename");
    cJSON *fullFileHashItem = cJSON_GetObjectItem(fileInfo, "fullFileHash");

    if(!chunkHashesItem || !filenameItem || !fullFileHashItem){
        printf("Missing required fields in file info\n");
        return;
    }

    int numChunks = cJSON_GetArraySize(chunkHashesItem);

    char outputPath[PATH_MAX];
    snprintf(outputPath, sizeof(outputPath), "%s/%s", directory, filenameItem->valuestring);

    FILE *outFile = fopen(outputPath, "wb");
    if(!outFile){ perror("fopen output file"); return; }

    printf("Downloading %s (%d chunks)...\n", filenameItem->valuestring, numChunks);

    //initialize full file hash context for final validation
    EVP_MD_CTX *fullFileCtx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(fullFileCtx, EVP_sha256(), NULL);

    struct timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    setsockopt(p2pSD, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    for(int i = 0; i < numChunks; i++){
        cJSON *chunkObj      = cJSON_GetArrayItem(chunkHashesItem, i);
        cJSON *chunkNameItem = cJSON_GetObjectItem(chunkObj, "chunkName");
        cJSON *chunkSizeItem = cJSON_GetObjectItem(chunkObj, "chunkSize");

        if(!chunkNameItem || !chunkSizeItem){ continue; }

        char *expectedHash  = chunkNameItem->valuestring;
        long expectedSize   = (long)chunkSizeItem->valuedouble;

        //send getChunk request to peer
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "requestType", "getChunk");
        cJSON_AddStringToObject(req, "chunkName", expectedHash);
        char *serialized = cJSON_PrintUnformatted(req);
        sendto(p2pSD, serialized, strlen(serialized), 0,
               (struct sockaddr *)&peerAddr, sizeof(peerAddr));
        free(serialized);
        cJSON_Delete(req);

        //allocate reassembly buffer sized to the expected chunk
        unsigned char *chunkBuf = malloc(expectedSize);
        if(!chunkBuf){ perror("malloc chunkBuf"); break; }
        memset(chunkBuf, 0, expectedSize);

        uint32_t totalFrags   = 0;
        uint32_t fragsReceived = 0;

        //receive fragments until we have them all
        unsigned char packet[8 + MAX_FRAG_PAYLOAD];
        while(1){
            struct sockaddr_in fromAddr;
            socklen_t fromLen = sizeof(fromAddr);
        

            int rc = recvfrom(p2pSD, packet, sizeof(packet), 0,
                              (struct sockaddr *)&fromAddr, &fromLen);
            
            if(rc < 0){
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    printf("Timeout waiting for chunk %d fragment\n", i);
                }else{
                    perror("recvfrom fragment");
                }
                free(chunkBuf);
                fclose(outFile);
                EVP_MD_CTX_free(fullFileCtx);
                return;
            }

            if(rc < 8) continue; //too small to be a valid fragment, skip

            //parse fragment header
            uint32_t fragIndex, fragTotal;
            memcpy(&fragIndex, packet,     4); fragIndex = ntohl(fragIndex);
            memcpy(&fragTotal, packet + 4, 4); fragTotal = ntohl(fragTotal);

            if(totalFrags == 0) totalFrags = fragTotal;
            

            //place fragment data into correct position in reassembly buffer
            size_t dataSize = rc - 8;
            size_t offset   = fragIndex * MAX_FRAG_PAYLOAD;
            if(offset + dataSize <= (size_t)expectedSize){
                memcpy(chunkBuf + offset, packet + 8, dataSize);
            }

            fragsReceived++;
            if(fragsReceived >= totalFrags) break; //got all fragments
        }

        //hash the reassembled chunk and compare to expected chunkName
        EVP_MD_CTX *chunkCtx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(chunkCtx, EVP_sha256(), NULL);
        EVP_DigestUpdate(chunkCtx, chunkBuf, expectedSize);
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen;
        EVP_DigestFinal_ex(chunkCtx, hash, &hashLen);
        EVP_MD_CTX_free(chunkCtx);

        char receivedHashHex[65];
        binaryToHex(hash, hashLen, receivedHashHex);

        if(strcmp(receivedHashHex, expectedHash) != 0){
            printf("ERROR: Chunk %d hash mismatch!\n  Expected: %s\n  Got:      %s\n",
                   i, expectedHash, receivedHashHex);
            free(chunkBuf);
            fclose(outFile);
            EVP_MD_CTX_free(fullFileCtx);
            return;
        }

        fwrite(chunkBuf, 1, expectedSize, outFile);
        EVP_DigestUpdate(fullFileCtx, chunkBuf, expectedSize);
        printf("  Chunk %d/%d OK (%.16s...)\n", i + 1, numChunks, expectedHash);
        free(chunkBuf);
    }

    fclose(outFile);

    //validate full file hash
    unsigned char fullHash[EVP_MAX_MD_SIZE];
    unsigned int fullHashLen;
    EVP_DigestFinal_ex(fullFileCtx, fullHash, &fullHashLen);
    EVP_MD_CTX_free(fullFileCtx);

    char fullHashHex[65];
    binaryToHex(fullHash, fullHashLen, fullHashHex);

    if(strcmp(fullHashHex, fullFileHashItem->valuestring) == 0){
        printf("\nFile downloaded and verified!\n  File: %s\n  Size: %ld bytes\n  Hash: %s\n  Saved to: %s\n",
            filenameItem->valuestring, 
            (long)cJSON_GetObjectItem(fileInfo, "fileSize")->valuedouble,
            fullHashHex, outputPath);
    } else {
        printf("\nERROR: Full file hash mismatch!\n  Expected: %s\n  Got:      %s\n",
               fullFileHashItem->valuestring, fullHashHex);
    }

    char drain[65536];
    struct sockaddr_in tmp;
    socklen_t tmpLen = sizeof(tmp);
    struct timeval drainTimeout = {0, 100000}; // 100ms
    setsockopt(p2pSD, SOL_SOCKET, SO_RCVTIMEO, &drainTimeout, sizeof(drainTimeout));
    while(recvfrom(p2pSD, drain, sizeof(drain), 0, (struct sockaddr*)&tmp, &tmpLen) > 0);
    struct timeval noTimeout = {0, 0};
    setsockopt(p2pSD, SOL_SOCKET, SO_RCVTIMEO, &noTimeout, sizeof(noTimeout));
}


//serve a chunk by fragmenting it into UDP packets
void serveChunkRequest(int p2pSD, const char *directory){
    char buffer[2048];
    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);

    int rc = recvfrom(p2pSD, buffer, sizeof(buffer) - 1, 0,
                      (struct sockaddr *)&fromAddr, &fromLen);
    if(rc < 0) return;
    buffer[rc] = '\0';

    cJSON *req = cJSON_Parse(buffer);
    if(!req) return;

    cJSON *requestTypeItem = cJSON_GetObjectItem(req, "requestType");
    if(!requestTypeItem || !cJSON_IsString(requestTypeItem) ||
       strcmp(requestTypeItem->valuestring, "getChunk") != 0){
        cJSON_Delete(req);
        return;
    }

    cJSON *chunkNameItem = cJSON_GetObjectItem(req, "chunkName");
    if(!chunkNameItem || !cJSON_IsString(chunkNameItem)){
        cJSON_Delete(req);
        return;
    }

    //open the chunk file and read all data into heap buffer
    char chunkPath[PATH_MAX];
    char chunkName[65];
    strncpy(chunkName, chunkNameItem->valuestring, 65);
    snprintf(chunkPath, sizeof(chunkPath), "%s/CHUNKS/%s",
             directory, chunkNameItem->valuestring);
    cJSON_Delete(req);

    FILE *chunkFile = fopen(chunkPath, "rb");
    if(!chunkFile){
        printf("serveChunkRequest: chunk not found: %s\n", chunkPath);
        return;
    }

    unsigned char *chunkData = malloc(CHUNK_SIZE + 1024);
    if(!chunkData){ perror("malloc"); return; }

    size_t totalBytes = fread(chunkData, 1, CHUNK_SIZE + 1024, chunkFile);
    fclose(chunkFile);

    
    uint32_t totalFrags = (uint32_t)((totalBytes + MAX_FRAG_PAYLOAD - 1) / MAX_FRAG_PAYLOAD);

    //send each fragmenta]
    unsigned char packet[8 + MAX_FRAG_PAYLOAD];
    for(uint32_t i = 0; i < totalFrags; i++){
        size_t offset    = i * MAX_FRAG_PAYLOAD;
        size_t fragSize  = totalBytes - offset;
        if(fragSize > MAX_FRAG_PAYLOAD) fragSize = MAX_FRAG_PAYLOAD;

        uint32_t netIndex = htonl(i);
        uint32_t netTotal = htonl(totalFrags);
        memcpy(packet,     &netIndex, 4);
        memcpy(packet + 4, &netTotal, 4);
        memcpy(packet + 8, chunkData + offset, fragSize);

        int sent = sendto(p2pSD, packet, 8 + fragSize, 0,
                          (struct sockaddr *)&fromAddr, sizeof(fromAddr));
        usleep(1000); // 1ms between fragments
        if(sent < 0){
            perror("serveChunkRequest: sendto fragment failed");
            free(chunkData);
            return;
        }
    }

    printf("Served chunk '%s'\n  -> to %s:%d (%zu bytes in %u fragments)\n",
       chunkName,
       inet_ntoa(fromAddr.sin_addr), ntohs(fromAddr.sin_port),
       totalBytes, totalFrags);

    free(chunkData);
}
