#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <cjson/cJSON.h>

int createSocketAndBind(int portNumber);
void joinMulticast(int sd, const char *multicastGroup);
void receiveStuff(int sd);
void parseMsg(char *buffer, char *clientIP, int clientPort);

void printData(cJSON* rootObj);

#define MAXPEERS (2048)
struct FileInfo *head = NULL;

struct FileInfo {
char filename[100];
char fullFileHash[65]; // SHA-256 hash is 64 hex digits + null terminator
char clientIP[MAXPEERS][INET_ADDRSTRLEN];
int clientPort[MAXPEERS];
int numberOfPeers;
struct FileInfo *next; // Pointer for linked list
};

int main(int argc, char *argv[]){
    int portNumber; 
    int sd;
    char *multicastGroup;
    
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

    receiveStuff(sd); //receive data from client over multicast group

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
void receiveStuff(int sd){
    char buffer[2056];
    struct sockaddr_in fromAddress;
    socklen_t fromLength;
    int rc;

    //line while receiving data and print each line that is being sent
    for(;;){

        fromLength = sizeof(fromAddress);

        //receive data and have error handling
        rc = recvfrom(sd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&fromAddress, &fromLength);
        if(rc < 0){
            printf("error on recvfrom\n");
            exit(1);
        }

        buffer[rc] = '\0'; //make sure cjson gets a valid c-string

        char fromIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &fromAddress.sin_addr, fromIP, sizeof(fromIP));

        printf("\n-------------------------------------------------\n");
        parseMsg(buffer, fromIP, ntohs(fromAddress.sin_port));    //send buffer to be parsed and printed 
    }
}


//function to print file info
void printList() {
    struct FileInfo *current = head;
    printf("\n========= Current File Registry =========\n");
    while (current != NULL) {
        printf("Filename:      %s\n", current->filename);
        printf("Full Hash:     %s\n", current->fullFileHash);
        printf("Number of Peers: %d\n", current->numberOfPeers);
        for (int i = 0; i < current->numberOfPeers; i++) {
            printf("  Peer %d:  %s:%d\n", i + 1, current->clientIP[i], current->clientPort[i]);
        }
        printf("-----------------------------------------\n");
        current = current->next;
    }
    printf("=========================================\n");
}

//function that properly adds a new file and its clients info
void registerFile(char *filename, char *fullFileHash, char *clientIP, int clientPort) {
    // search list for matching hash
    struct FileInfo *current = head;
    while (current != NULL) {
        if (strcmp(current->fullFileHash, fullFileHash) == 0) {
            // file exists, check for duplicate client
            for (int i = 0; i < current->numberOfPeers; i++) {
                if (strcmp(current->clientIP[i], clientIP) == 0 &&
                    current->clientPort[i] == clientPort) {
                    printf("Client %s:%d already registered for this file\n", clientIP, clientPort);
                    return;
                }
            }
            // not a duplicate, add client
            if (current->numberOfPeers < MAXPEERS) {
                strncpy(current->clientIP[current->numberOfPeers], clientIP, INET_ADDRSTRLEN);
                current->clientPort[current->numberOfPeers] = clientPort;
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
    strncpy(newNode->filename, filename, sizeof(newNode->filename));
    strncpy(newNode->fullFileHash, fullFileHash, sizeof(newNode->fullFileHash));
    strncpy(newNode->clientIP[0], clientIP, INET_ADDRSTRLEN);
    newNode->clientPort[0] = clientPort;
    newNode->numberOfPeers = 1;
    newNode->next = head;  // insert at front
    head = newNode;
}

//function to parse the received data and print it 
void parseMsg(char *buffer, char *clientIP, int clientPort) {
    cJSON *rootObj = cJSON_Parse(buffer);

    if (!rootObj) {
        printf("Invalid JSON received\n");
        return;
    }

    if (cJSON_IsObject(rootObj)) {
        // extract needed fields
        cJSON *filenameItem  = cJSON_GetObjectItem(rootObj, "filename");
        cJSON *hashItem      = cJSON_GetObjectItem(rootObj, "fullFileHash");

        if (filenameItem && hashItem &&
            cJSON_IsString(filenameItem) && cJSON_IsString(hashItem)) {
            registerFile(filenameItem->valuestring, hashItem->valuestring, clientIP, clientPort);
        } else {
            printf("Missing filename or fullFileHash in JSON\n");
        }

        printData(rootObj);   
        printList();          // print registry after every update
    }

    cJSON_Delete(rootObj);
}

void printData(cJSON* rootObj){
    char quotedKey[256];
    printf("---------------------------------\nParsed JSON data:\n");

    cJSON *item = NULL; //pointer to iterate of over key/value inside the rootObj
        //loop over every key/value pair inside rootObj
    cJSON_ArrayForEach(item, rootObj){
        //set key to whatever is the actual key string in the current key/value pair
        const char *key = item->string;
        //check if item is a JSON string and that valuestring exists
        if(cJSON_IsString(item) && item -> valuestring){
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s \"%s\"\n", quotedKey, item -> valuestring);
        }
        else if(cJSON_IsArray(item)){ //if item is an array then iterate over the values in the array and print each (chunk hashes)
            int size = cJSON_GetArraySize(item);
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s", quotedKey);
            for(int i = 0; i < size; i++){
                cJSON* chunkArray = cJSON_GetArrayItem(item, i);
                if(cJSON_IsString(chunkArray) && chunkArray -> valuestring){
                    printf("%20s\n", chunkArray -> valuestring);
                }
            }
        }
        //checks if value is a json number
        else if(cJSON_IsNumber(item)){
            snprintf(quotedKey, sizeof(quotedKey), "\"%s\"", key);
            printf("%-20s %d\n", quotedKey, item -> valueint);
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





