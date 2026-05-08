#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <cjson/cJSON.h>

#define MAXPEERS (2048)
struct FileInfo {
    char filename[100];
    char fullFileHash[65];
    char chunkHashes[1024][65]; // SHA-256 hash is 64 hex digits + null terminator
    int numberOfChunks;
    long chunkSizes[1024];
    char clientIP[MAXPEERS][INET_ADDRSTRLEN];
    int clientPort[MAXPEERS];
    int numberOfPeers;
    long fileSize;
    struct FileInfo *next; // Pointer for linked list
};

int createSocketAndBind(int portNumber);
void joinMulticast(int sd, const char *multicastGroup);
void receiveStuff(int sd, char *fromIP);
void parseMsg(char *buffer, char *clientIP, int sd, struct sockaddr_in clientAddr);
void printData(cJSON* rootObj);
void sendQueryResponse(struct FileInfo *head, int sd, struct sockaddr_in clientAddr);

struct FileInfo *head = NULL;


int main(int argc, char *argv[]){
    int portNumber; 
    int sd;
    char *multicastGroup;
    char fromIP[INET_ADDRSTRLEN];
    
    //verify valid arguments were sent
    if(argc < 3){
        printf("Usage is %s <multicast group> <port>\n", argv[0]);
        exit(1);
    }

    //set values for multicast and port and error check
    multicastGroup = argv[1];

    portNumber = atoi(argv[2]);
    if(portNumber <= 0 || portNumber > 65535){
        printf("Invalid port %d\n", portNumber);
        exit(1);
    }

    //create socket and bind and set value to sd
    sd = createSocketAndBind(portNumber);
    joinMulticast(sd, multicastGroup); //join multicastgroup sent in command line

    printf("UDP server is listening on port %d, in multicast group %s\n", 
        portNumber, multicastGroup);

    receiveStuff(sd, fromIP); //receive data from client over multicast group

    close(sd); //close socket
    return 0;
}

//function to create a socket and bind to it
int createSocketAndBind(int portNumber){
    int sd;
    int rc;
    struct sockaddr_in serverAddress;

    //print error for socket creation
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sd < 0){
        printf("socket error\n");
        exit(1);
    }

    int reuse = 1;
    setsockopt(sd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    //set required values
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(portNumber);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    //bind using the serveraddress with error check
    rc = bind(sd, (struct sockaddr*)&serverAddress, sizeof(serverAddress));
    if(rc < 0){
        printf("bind error\n");
        exit(1);
    }

    return sd;
}

//function to join multicastgroup that was given at command line
void joinMulticast(int sd, const char *multicastGroup){
    struct ip_mreq multicastRequest;
    multicastRequest.imr_multiaddr.s_addr = inet_addr(multicastGroup);
    multicastRequest.imr_interface.s_addr = INADDR_ANY;
    
    if(setsockopt(sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &multicastRequest, sizeof(multicastRequest)) < 0){
        printf("error on join multicast group\n");
        exit(1);
    }
}

//function to receive data from client, also calls function to print received data
void receiveStuff(int sd, char *fromIP){
    char buffer[65536];
    struct sockaddr_in fromAddress;
    socklen_t fromLength;
    int rc;

    //loop while receiving data and print each line that is being sent
    for(;;){

        fromLength = sizeof(fromAddress);

        //receive data and have error handling
        rc = recvfrom(sd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&fromAddress, &fromLength);
        if(rc < 0){
            printf("error on recvfrom\n");
            exit(1);
        }

        buffer[rc] = '\0'; //make sure cjson gets a valid string

        inet_ntop(AF_INET, &fromAddress.sin_addr, fromIP, INET_ADDRSTRLEN);

        printf("\n-------------------------------------------------\n");
        parseMsg(buffer, fromIP, sd, fromAddress); //send buffer to be parsed and printed 
    }
}


//function to print file info
void printList() {
    struct FileInfo *current = head;
    printf("\n========= Current File Registry =========\n");
    while (current != NULL) {
        printf("Filename:        %s\n", current->filename);
        printf("Full Hash:       %s\n", current->fullFileHash);
        printf("Number of Chunks: %d\n", current->numberOfChunks);
        printf("Number of Peers: %d\n", current->numberOfPeers);
        for (int i = 0; i < current->numberOfPeers; i++) {
            printf("  Peer %d:  %s:%d\n", i + 1, current->clientIP[i], current->clientPort[i]);
        }
        printf("-----------------------------------------\n");
        current = current->next;
    }
    printf("=========================================\n");
}

//function that adds a new file and its clients info
void registerFile(char *filename, char *fullFileHash, char chunkHashes[][65], long chunkSizes[], long fileSize, char *clientIP, int p2pPort, int numberOfChunks) {
    // search list for matching hash
    struct FileInfo *current = head;
    while (current != NULL) {
        if (strcmp(current->fullFileHash, fullFileHash) == 0) {
            // file exists, check for duplicate client
            for (int i = 0; i < current->numberOfPeers; i++) {
                if (strcmp(current->clientIP[i], clientIP) == 0 &&
                    current->clientPort[i] == p2pPort) {
                    printf("Client %s:%d already registered for this file\n", clientIP, p2pPort);
                    return;
                }
            }
            // not a duplicate, add client
            if (current->numberOfPeers < MAXPEERS) {
                strncpy(current->clientIP[current->numberOfPeers], clientIP, INET_ADDRSTRLEN);
                current->clientPort[current->numberOfPeers] = p2pPort;
                current->numberOfPeers++;
            }
            return;
        }
        current = current->next;
    }

    // hash not found, create new node
    struct FileInfo *newNode = malloc(sizeof(struct FileInfo));
    if (!newNode) {
        perror("malloc failed");
        exit(1);
    }
    strncpy(newNode->filename, filename, sizeof(newNode->filename) - 1);
    strncpy(newNode->fullFileHash, fullFileHash, sizeof(newNode->fullFileHash) - 1);
    newNode->fileSize = fileSize;
    strncpy(newNode->clientIP[0], clientIP, INET_ADDRSTRLEN);
    newNode->clientPort[0] = p2pPort;
    newNode->numberOfPeers = 1;

    
    for(int i = 0; i < numberOfChunks; i++){
        strncpy(newNode->chunkHashes[i], chunkHashes[i], 65);
        newNode->chunkSizes[i] = chunkSizes[i];
    }

   
    newNode->numberOfChunks = numberOfChunks;
    newNode->next = head;  // insert at front
    head = newNode;
}

//function to parse the received data and print it 
void parseMsg(char *buffer, char *clientIP, int sd, struct sockaddr_in clientAddr) {
    cJSON *rootObj = cJSON_Parse(buffer);

    if (!rootObj) {
        printf("Invalid JSON received\n");
        return;
    }

    if (cJSON_IsObject(rootObj)) {

        cJSON *requestType = cJSON_GetObjectItem(rootObj, "requestType");
        if(requestType == NULL){
            printf("Missing requestType field\n");
            cJSON_Delete(rootObj);
            return;
        }

        if(strcmp(requestType->valuestring, "query") == 0){
            sendQueryResponse(head, sd, clientAddr);
        }
        else if(strcmp(requestType->valuestring, "upload") == 0){

            // extract needed fields
            cJSON *filenameItem   = cJSON_GetObjectItem(rootObj, "filename");
            cJSON *hashItem       = cJSON_GetObjectItem(rootObj, "fullFileHash");
            cJSON *fileSizeItem   = cJSON_GetObjectItem(rootObj, "fileSize");
            cJSON *chunkArrayItem = cJSON_GetObjectItem(rootObj, "chunk_hashes");
            cJSON *p2pPortItem = cJSON_GetObjectItem(rootObj, "Port");
            if(!p2pPortItem){
                printf("Missing p2pPort, skipping\n");
                cJSON_Delete(rootObj);
                return;
            }
            int p2pPort = (int)p2pPortItem->valuedouble;
            

            int chunkArraySize = cJSON_GetArraySize(chunkArrayItem);
            char chunkHashes[1024][65];
            long chunkSizes[1024];

           
            for(int i = 0; i < chunkArraySize; i++){
                cJSON *chunkObj      = cJSON_GetArrayItem(chunkArrayItem, i);
                cJSON *chunkNameItem = cJSON_GetObjectItem(chunkObj, "chunkName");
                cJSON *chunkSizeItem = cJSON_GetObjectItem(chunkObj, "chunkSize");
                

                if(chunkNameItem && chunkSizeItem){
                    strncpy(chunkHashes[i], chunkNameItem->valuestring, 65);
                    chunkSizes[i] = (long)chunkSizeItem->valuedouble;
                }
            }

            if (filenameItem && hashItem && fileSizeItem &&
                cJSON_IsString(filenameItem) && cJSON_IsString(hashItem)) {
                registerFile(
                    filenameItem->valuestring, 
                    hashItem->valuestring, 
                    chunkHashes, 
                    chunkSizes, 
                    (long)fileSizeItem->valuedouble, 
                    clientIP, 
                    p2pPort,
                    chunkArraySize
                );
            } else {
                printf("Missing required fields in upload JSON\n");
            }

            printData(rootObj);
            printList();  // print registry after every update
        }
    }

    cJSON_Delete(rootObj);
}

void printData(cJSON* rootObj){
    char quotedKey[256];
    printf("---------------------------------\nParsed JSON data:\n");

    cJSON *item = NULL; //pointer to iterate over key/value inside the rootObj
    //loop over every key/value pair inside rootObj
    cJSON_ArrayForEach(item, rootObj){
        //set key to whatever is the actual key string in the current key/value pair
        const char *key = item->string;
        //check if item is a JSON string and that valuestring exists
        if(cJSON_IsString(item) && item->valuestring){
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s \"%s\"\n", quotedKey, item->valuestring);
        }
        else if(cJSON_IsArray(item)){ //if item is an array then iterate over values in array and print each
            int size = cJSON_GetArraySize(item);
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s (%d items)\n", quotedKey, size);
            for(int i = 0; i < size; i++){
                cJSON *arrayItem = cJSON_GetArrayItem(item, i);
                if(cJSON_IsObject(arrayItem)){
                    cJSON *chunkNameItem = cJSON_GetObjectItem(arrayItem, "chunkName");
                    cJSON *chunkSizeItem = cJSON_GetObjectItem(arrayItem, "chunkSize");
                    if(chunkNameItem && chunkSizeItem){
                        printf("  [%d] chunkName: %.16s... chunkSize: %ld\n", i,
                            chunkNameItem->valuestring, (long)chunkSizeItem->valuedouble);
                    }
                }
                else if(cJSON_IsString(arrayItem) && arrayItem->valuestring){
                    printf("  [%d] %s\n", i, arrayItem->valuestring);
                }
            }
        }
        //checks if value is a json number
        else if(cJSON_IsNumber(item)){
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s %d\n", quotedKey, item->valueint);
        }
        //checks if json value is a bool type
        else if(cJSON_IsBool(item)){
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s %s\n", quotedKey, cJSON_IsTrue(item) ? "true" : "false");
        }
        //if json value is null, "null" is printed
        else if(cJSON_IsNull(item)){
            printf("\"%-20s\" Null\n", key);
        }
    }
}

void sendQueryResponse(struct FileInfo *current, int sd, struct sockaddr_in clientAddr){

    cJSON *objectArray = cJSON_CreateArray();

    while(current != NULL){

        cJSON *jsonObject = cJSON_CreateObject();        
        cJSON_AddStringToObject(jsonObject, "filename", current->filename);
        cJSON_AddNumberToObject(jsonObject, "fileSize", current->fileSize);
        cJSON_AddStringToObject(jsonObject, "fullFileHash", current->fullFileHash);

        // build array with IP and Port objects
        cJSON *IPInfo = cJSON_CreateArray();
        for(int i = 0; i < current->numberOfPeers; i++){
            cJSON *ipPort = cJSON_CreateObject();
            cJSON_AddStringToObject(ipPort, "IP", current->clientIP[i]);
            cJSON_AddNumberToObject(ipPort, "Port", current->clientPort[i]);
            cJSON_AddItemToArray(IPInfo, ipPort);
        }
        cJSON_AddItemToObject(jsonObject, "IPInfo", IPInfo);

       
        cJSON_AddNumberToObject(jsonObject, "numberOfPeers", current->numberOfPeers);
        cJSON_AddNumberToObject(jsonObject, "numberOfChunks", current->numberOfChunks);

        
        cJSON *chunkArr = cJSON_CreateArray();
        for(int i = 0; i < current->numberOfChunks; i++){
            cJSON *chunkObj = cJSON_CreateObject();
            cJSON_AddStringToObject(chunkObj, "chunkName", current->chunkHashes[i]);
            cJSON_AddNumberToObject(chunkObj, "chunkSize", current->chunkSizes[i]);
            cJSON_AddItemToArray(chunkArr, chunkObj);
        }
        cJSON_AddItemToObject(jsonObject, "chunk_hashes", chunkArr);

        cJSON_AddItemToArray(objectArray, jsonObject);

        current = current->next;
    }

    cJSON *finalObj = cJSON_CreateObject();
    cJSON_AddStringToObject(finalObj, "requestType", "queryResponse");
    cJSON_AddItemToObject(finalObj, "files", objectArray);

    char *serializedObject = cJSON_PrintUnformatted(finalObj);
    sendto(sd, serializedObject, strlen(serializedObject), 0, (struct sockaddr *)&clientAddr, sizeof(clientAddr));
    printf("Sent query response\n");

    free(serializedObject);
    cJSON_Delete(finalObj);
}
