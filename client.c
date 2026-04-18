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
#include <cjson/cJSON.h>

#include <openssl/evp.h>

void sendStuff(char *buffer, int sd, struct sockaddr_in server_address);
void makeSocket(int *sd, char *argv[], struct sockaddr_in *server_address);
FILE *openFile(const char* fileName);
char *rtrim(char *s);
int parsePair(char **p, char *key,size_t keyCap, char *value, size_t valCap);
char *skipWhitespace(char *p);
char *readFileAndCreateJsonObjectandSerialize(char *buffer, FILE *fptr);

//new functions used for this lab
char* openDirectory(const char* directory, char* chunkDir);
cJSON* hashFileAndSave(const char* filePath, const char* fileName, const char* chunkDir);
void binaryToHex(unsigned char* hash, unsigned int length, char* output);

//lab4 functions
char* serializeJson(cJSON* jsonObj, char* buffer);

//chunk size 
#define CHUNK_SIZE (500 * 1024)

int main(int argc, char *argv[]){
    int sd;
    struct sockaddr_in server_address;
    char buffer[1024];

    char* directory;
    DIR* folder;
    struct dirent* entry;

    char* fileName;
    char filePath[PATH_MAX];
    cJSON* jsonObj = cJSON_CreateObject();

    if(argc < 4){
        printf("Usage: <IPaddr> <portNumber> <Directory Name>\n");
        exit(1);
    }

    makeSocket(&sd, argv, &server_address);


    directory = argv[3];


    folder = opendir(directory);
    if(folder == NULL){
        printf("Unable to read directory %s\n", directory);
        exit(1);
    }



    //create directory for the chunks to be saved in
    char chunkDir[PATH_MAX];
    snprintf(chunkDir, sizeof(chunkDir), "%s/CHUNKS", directory);
    if(mkdir(chunkDir, 0755) != 0){
        perror("mkdir failed");

    }

    while((entry = readdir(folder)) != NULL){

        if (strcmp(entry->d_name, ".") == 0 || 
            strcmp(entry->d_name, "..") == 0 || 
            strcmp(entry->d_name, "CHUNKS") == 0) continue;
        
        fileName = entry -> d_name;

        snprintf(filePath, sizeof(filePath), "%s/%s", directory, fileName);
        jsonObj = hashFileAndSave(filePath, fileName, chunkDir);
        sendStuff(serializeJson(jsonObj, buffer), sd, server_address);

    }

    closedir(folder);
    printf("Done\n");

    return 0;
}



//this function takes the filepath for every file in the directory, get the info to be stored in a json object 
//and also hashes the full file and the data, and it then stores each chunk as its own file in the CHUNKS folder
cJSON* hashFileAndSave(const char* filePath, const char* fileName, const char* chunkDir){

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

    //initialize the context for the full file hash **used what dave used (posted in canvas)
    EVP_MD_CTX *fileContext = EVP_MD_CTX_new();
    if(EVP_DigestInit_ex(fileContext, EVP_sha256(), NULL) != 1){
            EVP_MD_CTX_free(fileContext);
    }

    //iterate through the file's data 500kb at a time and hash each chunk, store the hashed chunk as a file
    //also hashes full file, updated incrementally 
    while((bytesRead = fread(buffer, 1, CHUNK_SIZE, fptr)) > 0){
        EVP_MD_CTX *chunkContext = EVP_MD_CTX_new();

        //initialize chunk context
        if(EVP_DigestInit_ex(chunkContext, EVP_sha256(), NULL) != 1){
            EVP_MD_CTX_free(chunkContext);
        }

        if(EVP_DigestUpdate(chunkContext, buffer, bytesRead) != 1){
            EVP_MD_CTX_free(chunkContext);
        }
        
        //this is to avoid rereading file, incrementally update full file hash
        if(EVP_DigestUpdate(fileContext, buffer, bytesRead) != 1){
            EVP_MD_CTX_free(fileContext);
        }

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen;

        if(EVP_DigestFinal_ex(chunkContext, hash, &hashLen) != 1){
            EVP_MD_CTX_free(chunkContext);
        }

        EVP_MD_CTX_free(chunkContext);

        

        char hashHex[65];
        binaryToHex(hash, hashLen, hashHex);
        
        //add chunk info to json object
        cJSON_AddItemToArray(chunkArray, cJSON_CreateString(hashHex));

        //write the hashed chunk to a new file in the subdirectory after making the subDir
        char chunkPath[PATH_MAX];
        snprintf(chunkPath, sizeof(chunkPath), "%s/%s", chunkDir, hashHex);

        FILE* chunkFptr = fopen(chunkPath, "wb");
        if (!chunkFptr) {
            perror("fopen chunk failed");
            exit(1);
        }

        fwrite(hash, 1, hashLen, chunkFptr);
        fclose(chunkFptr);
        numOfChunks++;

    }


    unsigned char fileHash[EVP_MAX_MD_SIZE];
    unsigned int fileHashLen;
    if(EVP_DigestFinal_ex(fileContext, fileHash, &fileHashLen) != 1){
            EVP_MD_CTX_free(fileContext);
    }

    EVP_MD_CTX_free(fileContext);

    char fileHashHex[65];
    binaryToHex(fileHash, fileHashLen, fileHashHex);

    //add hashed full file and chunk info into obj and return that obj
    cJSON_AddStringToObject(jsonObject, "filename", fileName);
    cJSON_AddNumberToObject(jsonObject, "fileSize", fileSize);
    cJSON_AddNumberToObject(jsonObject, "numberOfChunks", numOfChunks);
    cJSON_AddItemToObject(jsonObject, "chunk_hashes", chunkArray);
    cJSON_AddStringToObject(jsonObject, "fullFileHash", fileHashHex);

    fclose(fptr);
    return jsonObject;

}

char* serializeJson(cJSON* jsonObj, char* buffer){

    char* serializedObject = cJSON_PrintUnformatted(jsonObj);
    strcpy(buffer, serializedObject);
    free(serializedObject);
    return buffer;
}

void makeSocket (int *sd, char* argv[], struct sockaddr_in *server_address){
    
    struct sockaddr_in inaddr; //temp value for validation checking 
    int portNumber; //from cmd line
    char serverIP[50]; //ip from cmd line


    //checks for valid ip address
    if(!inet_pton(AF_INET, argv[1], &inaddr)){
        printf("error, bad ip address\n");
        exit(1);
    }

    //creat a socket
    *sd = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(*sd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    //error checking on socket creation
    if(*sd == -1){
        perror("socket");
        exit(1);
    }

    

    portNumber = strtol(argv[2], NULL, 10);

    //validate port number
    if((portNumber > 65535) || (portNumber < 0)){
        printf("You entered an invalid socker number/n");
        exit(1);
    }

    //copy the ip address from cmd line 
    strcpy(serverIP, argv[1]); 
    //fill in the address data structure
    server_address->sin_family = AF_INET; //use AF INET addrs
    server_address->sin_port = htons(portNumber); //convert port number
    server_address->sin_addr.s_addr = inet_addr(serverIP); //convert IP addr
}

//gets the name of file from user, opens the file, and returns the file descriptor
FILE * openFile(const char* fileName){
    FILE * fptr = NULL;

    fptr = fopen(fileName, "rb");
    if(fptr == NULL){
        printf("Error opening file, try again\n");
    }
    return fptr;
}

//this function converts the binary hash into hex to be used for json output and filename for each hash
void binaryToHex(unsigned char* hash, unsigned int length, char* output){
    for(unsigned int i = 0; i < length; i++){
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[length * 2] = '\0';
}


void sendStuff(char *buffer, int sd, struct sockaddr_in server_address){

    if(!buffer){
        printf("done\n");
        return;
    }
    int sent = 0;
    printf("%s\n", buffer);
    sent = sendto(sd, buffer, strlen(buffer), 0, (struct sockaddr *) &server_address, sizeof(server_address));
    if(sent < 0){
        perror("sent less than 0");
        exit(1);
    }   
}


