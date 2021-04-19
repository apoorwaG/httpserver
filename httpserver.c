#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>    // write
#include <string.h>    // memset
#include <stdlib.h>    // atoi
#include <assert.h> 
#include <ctype.h>     // alpha/numeric
#include <errno.h>     // errno
#include <pthread.h>   // pthreads
#include <semaphore.h> // semaphores

#define BUFFERSIZE 1000
#define NUMBER_STRINGS 100
#define STRING_LENGTH 1000
#define BUFFER_SIZE 4096

extern int errno;
extern char* optarg;
extern int optind, opterr, optopt;

// Global variables
sem_t empty;    // counting semaphore for number of empty slots
sem_t full;     // counting semaphore for number of filled slots
sem_t sem;      // locking semaphore used for critical sections

ssize_t array[BUFFERSIZE];  // this global array will be shared between the producer and comsumers(threads)
int in = 0;             // index for inserting into the array
int out = 0;            // index for extracting from the array

int firstLogFD;
int logFlag = -1;       // global variable will let the threads know if logging is performed
int numClients = 0;     // numClients will hold total number of client requests so far
int numErrors = 0;      // numErrors will hold total number of errors so far

// int log_fd;                 // file descriptor of log_file if it is created: shared among threads
char* logFile = NULL;           // log file name
ssize_t universal_offset = 0;   // offset to start writing the logFiles -> each thread changes the offset
                            // so the following threads start writing at that offset
// returns 1 if operator syntax correct and 0 else
// argument: first token of the first line of the request
int opcheck(char op[]){
    int case1 = strcmp(op, "GET");
    int case2 = strcmp(op, "PUT");
    int case3 = strcmp(op, "HEAD");
    if(case1 == 0 || case2 == 0 || case3 == 0){
        return 1;
    }
    else{
        return 0;
    }
}

// returns 1 if HTTP/1.1 syntax correct and 0 else
// argument: 3rd token of the first line in the request
int httpCheck(char http[]){
    if(strcmp(http, "HTTP/1.1") == 0){
        return 1;
    }
    else{
        return 0;
    }
}

// returns 1 if header format is correct -> name: value
// argument: header string -> name: value\r\n
int headerCheck(char line[]){
    int keyOK = 0;
    char key[30];
    int token = sscanf(line, "%s %*s", key);
    size_t length = strlen(key);
    if(token == 1 && key[length - 1] == ':'){                   // first check if header format is key: val
        keyOK = 1;
        if(strcmp(key, "Content-Length:") == 0){                // if the header is Content-Length: val
            int contentVal;
            int token1 = sscanf(line, "%*s %d", &contentVal);
            if(token1 == 1 && contentVal >= 0){
                keyOK = 1;
            }
            else{                                               // if Content-Length header syntax is incorrect
                keyOK = 0;
            }
        }
    }
    else{                                                       // if header fomrmat isn't correct
        keyOK = 0;
    }
    return keyOK;
}


// returns 1 if file name is acceptable and 0 else
// argument: 2nd token of the first line of the request
int fileNameCheck(char fileName[]){
    int valid = 1;
    int fileLength = strlen(fileName);
    if(fileLength > 27){
        return 0;
    }
    else{
        for(int i = 0; i < fileLength; i++){
            if(isalpha(fileName[i]) || isdigit(fileName[i]) || fileName[i] == '-' || fileName[i] == '_'){
                valid = 1;
            }
            else{
                valid = 0;
                break;
            }
        }
    }
    return valid;
}


// returns 1 if syntax is correct and 0 else
// argument: string to check for errors, the position of that string (in an array), and total # strings to process
int checkSyntax(char line[], uint8_t lineNum, uint8_t totalLines){
    int syntaxOK;
    if(lineNum == 1){                           // if we're dealing with the first line
        char operator[20];
        char file[400];
        char http[10];
        int tokens = sscanf(line, "%s /%s %s\r", operator, file, http);
        if(tokens == 3){
            int opOK = opcheck(operator);
            int fileOK = fileNameCheck(file);
            int httpOK = httpCheck(http);
            if(opOK == 0 || fileOK == 0 || httpOK == 0){        // if either operator or file Name or http have broken syntax
                syntaxOK = 0;                   // syntax is incorrect
            }
            else{
                syntaxOK = 1;                   // syntax correct for op, filename, http -> syntaxOK 1
            }
        }
        else{                                   // syntax incorrect if first line doesn't have 3 correct tokens
            syntaxOK = 0;
        }
    }
    else{               // if we're not dealing with the first line 
        syntaxOK = 0;   // assume syntax not ok
        if(lineNum == totalLines){                  // the last line should be a blank line
            if(strcmp(line, "blank line") == 0){
                syntaxOK = 1;                       // update: syntaxOK 1 if case true
            }
        }
        else{
            int headerOK = headerCheck(line);
            if(headerOK == 1){
                syntaxOK = 1;                       // update: syntaxOK 1 if case true
            }
        }
    }
    return syntaxOK;
}

// returns 0 if PUT, 1 if GET, and 2 if HEAD
// argument: first line of the request
int getOP(char request[]){
    char operator[10];
    int token = sscanf(request, "%s %*s %*s\r", operator);
    if(token == 1 && strcmp(operator, "PUT") == 0){
        return 0;
    }
    else if(token == 1 && strcmp(operator, "GET") == 0){
        return 1;
    }
    else if(token == 1 && strcmp(operator, "HEAD") == 0){
        return 2;
    }
    else{
        return -1;
    }
}

// returns the value of the Content-Length header (int)
// argument: header string -> key: value
ssize_t getContentLenth(char line[]){
    ssize_t contentLength;
    int token = sscanf(line, "Content-Length: %ld", &contentLength);
    if(token == 1){
        return contentLength;
    }
    else{
        return -1;
    }
}
// initializes the lines array with the request, divided into each line
void initializeArray(char * lines[], char * a_buffer, int * numLines){
    // Start splitting the request into lines -> split on '\n'
    char * saveptr = a_buffer;
    const char * splitOn = "\n";
    char * line = strtok_r(a_buffer, splitOn, &saveptr);

    int index = 0;
    while(line != NULL){
        if(line[0] == '\r'){
            char * blank = "blank line";    // for \r\n\r\n, the array will contain "blank line"
            lines[index] = blank;
            ++index;
            break;                          // \r\n\r\n is the end of the request
        }
        lines[index] = line;
        ++index;
        line = strtok_r(NULL, splitOn, &saveptr);
    }
    *numLines = index;                      // numLines will hold the number of elements in the array
}

// returns the number of digits in a given number
ssize_t countDigits(ssize_t contentL){
    ssize_t n = 0;
    while(contentL != 0){
        contentL /= 10;
        ++n;
    }
    return n;
}

// returns the size of the data in bytes that will be stored in the logfile
ssize_t getBodySpace(ssize_t contentL){
    // exactly 20 bytes in each line
    ssize_t num_lines = 0;
    ssize_t bodySpace = 0;
    if(contentL % 20 == 0){
        num_lines = contentL/20;
        bodySpace = 69 * num_lines; 
    }
    else{
        num_lines = contentL/20;
        bodySpace = num_lines * 69 + ((contentL % 20)*3 + 9);
    }

    return bodySpace;
}

ssize_t getSpaceNeeded(char firstLine[], uint16_t code){

    char fileName[500];
    char request[20];
    sscanf(firstLine, "%s /%s %*s", request, fileName);
    ssize_t totalSpace = 41 + strlen(fileName) + countDigits(code) + strlen(request);
    return totalSpace;
}

// args: responseCode, content-Length of file, fileName and opnum
ssize_t getLoggingSpace(uint16_t responseCode, ssize_t contentL, char fileN[], int opnum){
    ssize_t totalSpace = 0;
    ssize_t headerSpace = 0;
    ssize_t bodySpace = 0;

    // if PUT or GET
    if(opnum == 0 || opnum == 1){
        // if operation was succesful
        if(responseCode == 201 || responseCode == 200){
            if(contentL == 0){
                headerSpace = 14 + strlen(fileN) + 1;
            }
            else{
                headerSpace = (14 + countDigits(contentL) + strlen(fileN));
            }
            bodySpace = getBodySpace(contentL);
            totalSpace = headerSpace + bodySpace + 9;
        }
        // if operation wasn't successful
        else{
            totalSpace = 47 + strlen(fileN);
        }
    }
    // if HEAD
    else if(opnum == 2){
        // if successful HEAD
        if(responseCode == 200){
            if(contentL == 0){
                totalSpace = 24 + 1 + strlen(fileN);
            }
            else{
                totalSpace = 24 + countDigits(contentL) + strlen(fileN);
            }
        }
        // unsuccessful HEAD
        else{
            totalSpace = 48 + strlen(fileN);
        }
    }

    return totalSpace;

}


void healthLogger(int logFD, uint16_t responseCode, ssize_t contentL, ssize_t start, ssize_t totalErrors, ssize_t totalClients){
    
    // logFD is already open

    int numBytes = 0;
    char lineMarker[9];
    char hexByte[5];
    ssize_t writeAt = start;

    if(responseCode == 200){
        char header[100];
        snprintf(header, 50, "GET /healthcheck length %ld\n", contentL);
        pwrite(logFD, header, strlen(header), writeAt);
        writeAt += strlen(header);

        snprintf(lineMarker, 9, "%08d", numBytes);
        pwrite(logFD, lineMarker, strlen(lineMarker), writeAt);
        writeAt += strlen(lineMarker);

        char data[200];
        snprintf(data, 100, "%ld\n%ld", totalErrors, totalClients);
        for(size_t i = 0; i < strlen(data); i++){

            if(i == strlen(data) - 1){
                uint8_t byte = data[i];
                snprintf(hexByte, 5, " %02x\n", byte);
                pwrite(logFD, hexByte, strlen(hexByte), writeAt);
                writeAt = writeAt +  strlen(hexByte);
                break;
            }

            uint8_t byte = data[i];
            snprintf(hexByte, 4, " %02x", byte);
            pwrite(logFD, hexByte, strlen(hexByte), writeAt);
            writeAt += strlen(hexByte);
        }
    }
    else{

    }
    char endline[] = "========\n";
    pwrite(logFD, endline, strlen(endline), writeAt);

    // close(logFD);
}

void invalidRequestLogger(int logFD, uint16_t responseCode, ssize_t start_offset, char firstLine[]){

    char fileN[200];
    char method[20];
    char HTTP[20];

    sscanf(firstLine, "%s /%s %s\r\n", method, fileN, HTTP);
    char header[300];
    snprintf(header, 200, "FAIL: %s /%s %s --- response %u\n========\n", method, fileN, HTTP, responseCode);
    pwrite(logFD, header, strlen(header), start_offset);

    // close(logFD);

}

void logger(int logFD, uint16_t responseCode, ssize_t contentL, char fileName[], int opnum, ssize_t start, char firstLine[]){

    int loggingFD = logFD;

    int READSIZE = 20;

    ssize_t writeAt = start;

    ssize_t fileFD;

    // for successful requests
    if(responseCode == 200 || responseCode == 201){
        // print the header first
        char header[400];
        if(opnum == 0){
            snprintf(header, 200, "PUT /%s length %ld\n", fileName, contentL);
        }
        else if(opnum == 1){
            snprintf(header, 200, "GET /%s length %ld\n", fileName, contentL);
        }
        else if(opnum == 2){
            snprintf(header, 200, "HEAD /%s length %ld\n", fileName, contentL);
        }
        pwrite(loggingFD, header, strlen(header), writeAt);
        writeAt += strlen(header);

        // for PUT and GET, also write the body to logfile
        if(opnum == 0 || opnum == 1){

            char buffer[20];
            char hexByte[6];
            fileFD = open(fileName, O_RDONLY);
            ssize_t bytes_read = read(fileFD, buffer, READSIZE);
            int numBytes = 0;
            char lineMarker[9];

            while(bytes_read > 0){
                snprintf(lineMarker, 9, "%08d", numBytes);
                numBytes += 20;
                pwrite(loggingFD, lineMarker, strlen(lineMarker), writeAt);
                writeAt += strlen(lineMarker);

                for(ssize_t i = 0; i < bytes_read; i++){
                    if(i == bytes_read - 1){
                        uint8_t byte = buffer[i];
                        snprintf(hexByte, 6, " %02x\n", byte);
                        pwrite(loggingFD, hexByte, strlen(hexByte), writeAt);
                        writeAt = writeAt +  strlen(hexByte);
                        break;
                    }
                    uint8_t byte = buffer[i];
                    snprintf(hexByte, 5, " %02x", byte);
                    pwrite(loggingFD, hexByte, strlen(hexByte), writeAt);
                    writeAt += strlen(hexByte);
                }
                bytes_read = read(fileFD, buffer, READSIZE);
            }
        }
        // if healthcheck is being logged
        char endline[] = "========\n";
        pwrite(loggingFD, endline, strlen(endline), writeAt);
        if(opnum == 0 || opnum == 1){
            close(fileFD);
        }
    }
    // failed requests:
    else{
        char op[200];
        char fileN[200];
        char HTTP[20];
        sscanf(firstLine, "%s /%s %s\r\n", op, fileN, HTTP);
        char header[500];
        snprintf(header, 300, "FAIL: %s /%s %s --- response %u\n========\n", op, fileN, HTTP, responseCode);
        pwrite(loggingFD, header, strlen(header), writeAt);
    }
    
    // close(loggingFD);
}

// each thread executes this function in parallel
// arg: client socket
// function will use the socket, get the request, and execute it
void processRequest(ssize_t client){
    ssize_t client_sockd = client;
    // buffer to hold headers + (possible body)
    char request[BUFFER_SIZE + 1];
    memset(request, 0, BUFFER_SIZE + 1);

    printf("Client number: %ld\n", client_sockd);

    ssize_t bytes = recv(client_sockd, request, BUFFER_SIZE, 0);
    if(bytes == -1){
        close(client_sockd);
        return;
    }
    // dprintf(client_sockd, "Bytes received: %ld\n", bytes);

    // find where the headers end
    char * body = strstr(request, "\r\n\r\n");
    body += 4;
    ssize_t initial_read = 0;

    // if there is data after the headers
    if(body != NULL){
        // printf("After the headers: %s\nLength of excess data: %ld\n", body, strlen(body));
        initial_read = strlen(body);
    }

    // this array of string will contain the request and headers (no data)
    char * lines[NUMBER_STRINGS];

    // holds the number of elements in the string array
    int index;
    // lines is initialized with the request -> each index holds a line
    initializeArray(lines, request, &index);

    // syntaxCorrect = 1 if correct and 0 else
    // checks the syntax of every line
    int syntaxCorrect;
    for(uint8_t j = 0; j < index; j++){
        syntaxCorrect = checkSyntax(lines[j], j+1, index);  // arguments: line, line number, and total # lines
        if(syntaxCorrect == 0){
            break;  
        }
    }

    // At this point, syntaxCorrect is known: if 1 then begin processing the request
    uint16_t responseCode;      // will hold response code -> 200 or 404 ....
    char * responseString;      // will hold response string -> OK or Created ....


    printf("Syntax Correct: %d\n", syntaxCorrect);

    // File descriptor for logfile
    int loggingFD;
    if(logFlag == 1){
        // set loggingFD to global variable firstLogFD
        loggingFD = firstLogFD;
    }

    // sem_wait(&sem);
    // char tempName[200];
    // sscanf(lines[0], "%*s /%s %*s", tempName);
    // if(strcmp(tempName, "healthcheck") == 0){
    //     --numClients;
    // }
    // sem_post(&sem);


    if(syntaxCorrect == 1){
        // fileName set
        char fileName[28];
        sscanf(lines[0], "%*s /%s %*s", fileName);
        
        // 0 = PUT, 1 = GET, 2 = HEAD
        int opnum = getOP(lines[0]);

        // check for healtcheck errors right away
        if(strcmp(fileName, "healthcheck") == 0 && (opnum == 0 || opnum == 2)){
            dprintf(client_sockd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
            sem_wait(&sem);
            ++numErrors;
            sem_post(&sem);
            opnum = -1;
            if(logFlag == 1){
                sem_wait(&sem);
                ssize_t spaceRequired = getSpaceNeeded(lines[0], 403);
                ssize_t startingOffset = universal_offset;
                universal_offset += spaceRequired;
                sem_post(&sem);
                invalidRequestLogger(loggingFD, 403, startingOffset, lines[0]);
            }
        }
        if(strcmp(fileName, "healthcheck") == 0 && (logFlag == -1)){
            dprintf(client_sockd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            opnum = -1;
        }

        // PUT request*************************************************************************************
        if(opnum == 0){

            ssize_t contentLength = -1;
            for(int k = 1; contentLength == -1; k++){
                contentLength = getContentLenth(lines[k]);
            }

            // printf("Content-Length: %ld\n", contentLength);
            char buffer_a[BUFFER_SIZE + 1];
            ssize_t fd;
            // first, open file assuming it hasn't been created
            fd = open(fileName, O_RDWR | O_CREAT | O_TRUNC, 0664);
            // succesfully opened
            if(fd != -1){
                ssize_t bytes_read;
                ssize_t bytes_written;

                // if body partially included in headers, finish writing partial data first
                if(initial_read > 0){
                    bytes_written = write(fd, body, strlen(body));
                }
                
                ssize_t bytes_remaining = contentLength - initial_read;
                
                while(bytes_remaining > 0 && (bytes_read = recv(client_sockd, buffer_a, BUFFER_SIZE, 0)) > 0){
                    
                    bytes_written = write(fd, buffer_a, bytes_read);
                    bytes_remaining = bytes_remaining - bytes_read;
                    if(bytes_written == -1)
                        break;
                }
                // error while I/O
                if(bytes_read == -1 || bytes_written == -1){
                    responseCode = 500;
                    responseString = "Internal Server Error";
                    sem_wait(&sem);
                    ++numErrors;
                    sem_post(&sem);
                }

                if(bytes_read != -1 && bytes_written != -1 && bytes_remaining == 0){
                    // printf("Successful\n");
                    responseCode = 201;
                    responseString = "Created";
                }

                close(fd);
            }
            // Error in opening the file -> permission changed
            else{
                int errnum = errno;
                if(errnum == 13){
                    responseCode = 403;
                    responseString = "Forbidden";
                    sem_wait(&sem);
                    ++numErrors;
                    sem_post(&sem);
                }
            }
            

            
            dprintf(client_sockd, "HTTP/1.1 %u %s\r\nContent-Length: 0\r\n\r\n", responseCode, responseString);

            // if we are also logging the file
            if(logFlag == 1){
                sem_wait(&sem);     // critical region
                ssize_t spaceNeeded = getLoggingSpace(responseCode, contentLength, fileName, opnum);
                ssize_t start_offset = universal_offset;
                universal_offset += spaceNeeded;
                sem_post(&sem);     // leave critical region
                logger(loggingFD, responseCode, contentLength, fileName, opnum, start_offset, lines[0]);

            }


        }
        // GET/HEAD request********************************************************************************
        else if(opnum == 1 || opnum == 2){
            ssize_t contentLength = 0;
            ssize_t fd2 = -1;
            int healthCheck;

            // dprintf(client_sockd, "Bytes1 received: %ld\n", bytes);


            if(strcmp(fileName, "healthcheck") == 0){
                healthCheck = 1;
            }
            else{
                healthCheck = 0;
            }

            if(healthCheck == 0){
                fd2 = open(fileName, O_RDONLY);         // file descriptor for the file we want to read from
            }

            // dprintf(client_sockd, "Bytes2 received: %ld\n", bytes);

            if(healthCheck == 0 && fd2 != -1){
                // get the Content-Length of the file
                struct stat len_buffer;
                int8_t status;
                status = fstat(fd2, &len_buffer);
                if(status == 0){
                    contentLength = len_buffer.st_size;
                    responseCode = 200;
                    responseString = "OK";
                }
                else{
                    responseCode = 500;
                    responseString = "Internal Server Error";
                }
                dprintf(client_sockd, "HTTP/1.1 %u %s\r\nContent-Length: %ld\r\n\r\n", responseCode, responseString, contentLength);

                // Also send the data as it is a GET request
                if(opnum == 1 && responseCode == 200){
                    ssize_t bytes_written;
                    char buffer[BUFFER_SIZE];
                    ssize_t bytes_read = read(fd2, buffer, BUFFER_SIZE);
                    while(bytes_read > 0){
                        bytes_written = write(client_sockd, buffer, bytes_read);
                        if(bytes_written == -1){
                            break;
                        }
                        bytes_read = read(fd2, buffer, BUFFER_SIZE);
                    }
                    if(bytes_read == -1 || bytes_written == -1){
                        responseCode = 500;
                        responseString = "Internal Server Error";
                        sem_wait(&sem);
                        ++numErrors;
                        sem_post(&sem);
                    } 
                }
                                   
            }   
            // fd2 == -1: permission denied or file doesn't exist
            else if(healthCheck == 0 && fd2 == -1){
                int errnum = errno;
                if(errnum == 2){
                    responseCode = 404;
                    responseString = "Not found";
                }
                else if(errnum == 13){
                    responseCode = 403;
                    responseString = "Forbidden";
                }
                else{
                    responseCode = 500;
                    responseString = "Internal Server Error";
                }
                sem_wait(&sem);
                numErrors++;
                sem_post(&sem);

                dprintf(client_sockd, "HTTP/1.1 %u %s\r\nContent-Length: 0\r\n\r\n", responseCode, responseString);

            }

            if(healthCheck == 0){
                close(fd2);
            }


            // if logging enabled
            if(logFlag == 1){
                // logging a regular file

                if(healthCheck == 0){
                    sem_wait(&sem);     // critical region
                    ssize_t spaceNeeded = getLoggingSpace(responseCode, contentLength, fileName, opnum);
                    ssize_t start_offset = universal_offset;
                    universal_offset += spaceNeeded;
                    sem_post(&sem);     // leave critical region
                    logger(loggingFD, responseCode, contentLength, fileName, opnum, start_offset, lines[0]);
                }

                // logging healthcheck
                else if(healthCheck == 1){

                    ssize_t start_offset;
                    ssize_t cLength = 0;        // length of healthcheck "file"

                    sem_wait(&sem);
                    ssize_t totalErrors = numErrors;
                    ssize_t totalClients = numClients;

                    ssize_t size_clients = countDigits(totalClients);
                    ssize_t size_errors = countDigits(totalErrors);

                    if(size_clients == 0 && size_errors == 0){
                        cLength = 3;
                    }
                    else if(size_errors == 0 && size_clients != 0){
                        cLength = 2 + size_clients;
                    }
                    else{
                        cLength = size_errors + size_clients + 1;
                    }

                    ssize_t totalSpace = getLoggingSpace(200, cLength, "healthcheck", opnum);
                    start_offset = universal_offset;
                    universal_offset += totalSpace;

                    sem_post(&sem);

                    dprintf(client_sockd, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n%ld\n%ld", cLength, totalErrors, totalClients);

                    healthLogger(loggingFD, 200, cLength, start_offset, totalErrors, totalClients);

                }
            }
        }

    }
    // Syntax Incorrect
    // Error 400
    // Send bad request message to Client
    else if(syntaxCorrect == 0){
        sem_wait(&sem);
        ++numErrors;
        sem_post(&sem);
        responseCode = 400;
        responseString = "Bad Request";

        dprintf(client_sockd, "HTTP/1.1 %u %s\r\nContent-Length: 0\r\n\r\n", responseCode, responseString);

        
        if(logFlag == 1){

            int opNum = getOP(lines[0]);

            if(opNum == 0 || opNum == 1 || opNum == 2){
                char fileName[500];
                sscanf(lines[0], "%*s /%s %*s", fileName);
                sem_wait(&sem);     // critical region
                ssize_t spaceNeeded = getLoggingSpace(responseCode, 0, fileName, opNum);
                ssize_t start_offset = universal_offset;
                universal_offset += spaceNeeded;
                sem_post(&sem);     // leave critical region
                logger(loggingFD, responseCode, 0, fileName, opNum, start_offset, lines[0]);
            }

            // invalid request that not put, get or head
            else{

                sem_wait(&sem);

                ssize_t spaceNeeded = getSpaceNeeded(lines[0], responseCode);

                ssize_t start_offset = universal_offset;
                universal_offset += spaceNeeded;
                sem_post(&sem);
                char fileName1[200];
                // char request[20];
                sscanf(lines[0], "%*s /%s %*s", fileName1);
                invalidRequestLogger(loggingFD, responseCode, start_offset, lines[0]);

                // logger(loggingFD, responseCode, 0, fileName1, opNum, start_offset, lines[0]);
            }
        }
    }

    printf("Finished processing request\n\n");

    // printf("Finished socket: %ld\n", client_sockd);
    ++numClients;
    close(client_sockd);
    // printf("Closed socket: %ld\n", client_sockd);

}

// consumer function that extracts info from the array when someting is inserted
void* executeWorker(void * arg){
    int threadId = *(int *)arg;
    printf("Enter thread: %d\n", threadId);
    ssize_t socket;
    while(1){
        // printf("Thread %d waits on producer\n", threadId);
        //
        printf("Thread %d sleeping\n", threadId);
        sem_wait(&full);                       // wait until at least 1 slot in the array is filled 
        sem_wait(&sem);                        // thread either enters critical section or sleeps 
        socket = array[out];
        out = (out + 1) % BUFFERSIZE;
        printf("Thread %d will handle client %ld\n", threadId, socket);
        sem_post(&sem);                        // thread leaves critical section
        sem_post(&empty);                      // up the empty semaphore since we used one item 
        //printf("Thread %d off with client %d\n", threadId, socket); 
        processRequest(socket);
        // printf("Thread leaves critical section\n");
    }
}


int main(int argc, char** argv) {

    if(argc < 2){
        printf("Not enough arguments!\n");
        return EXIT_FAILURE;
    }

    int threadFlag = -1;       // flag will be set to 1 if num of threads specified
    char* threads_num = NULL;  // will hold the string representation of num_threads if specified
    char* port = NULL;         // will hold port string if specified

    int numThreads = 0;        // will hold the integer representation of number of threads specified;

    printf("Argument Count: %d\n", argc);


    // getop() switch case style based on TA Michael's Discussion Section 
    int c;
    while((c = getopt(argc, argv, "N:l:")) != -1){

        if(c == 'N'){
            threads_num = optarg;
        }
        else if(c == 'l'){
            logFile = optarg;
        }

        // switch(c){
        //     case 'N':
        //         threads_num = optarg;
        //         break;
        //     case 'l':
        //         logFile = optarg;
        //         break;
        //     case '?':
        //         if(optopt == 'N'){
        //             fprintf(stderr, "Option -%c requires an argument.\n", optopt);
        //         }
        //         else if(isprint(optopt)){
        //             fprintf(stderr, "Unknown option -%c.\n", optopt);
        //         }
        //         else{
        //             fprintf(stderr, "Unknown option character \\x%x.\n", optopt);
        //         }

        //         return EXIT_FAILURE;
        //     default:
        //         abort();
        // }
    }

    // numThreads isn't null: -N specified but may be invalid
    if(threads_num != NULL){
        if(threads_num[0] == '-' || atoi(threads_num) == 0){
            fprintf(stderr, "Invalid argument for -N\n");
            // return EXIT_FAILURE;
            threadFlag = -1;
        }
        else{
            threadFlag = 1;
            numThreads = atoi(threads_num);
            if(numThreads < 2){
                // fprintf(stderr, "Invalid argument for -N\n");
                threadFlag = -1;
            }
        }
        printf("Num threads: %d\n", numThreads);
    }

    // logfile isn't null: -l specified and name is stored in logFile
    if(logFile != NULL){
        logFlag = 1;
        firstLogFD = open(logFile, O_TRUNC | O_CREAT | O_WRONLY, 0600);
        // close(log_fd);
        printf("Log file: %s\n", logFile);
    }

    // if port argument isn't specified, argc = optind
    // else, optind = argc - 1
    if(optind != argc - 1){
        printf("Not enough arguments! Enter port number\n");
        return EXIT_FAILURE;
    }

    // fail if invalid port number given
    port = argv[optind];
    if(atoi(port) == 0){
        printf("Invalid port!\n");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(atoi(port));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    socklen_t addrlen = sizeof(server_addr);

    //  Create server socket
    int server_sockd = socket(AF_INET, SOCK_STREAM, 0);

    // Need to check if server_sockd < 0, meaning an error
    if (server_sockd < 0) {
        perror("socket");
    }

    // Configure server socket
    int enable = 1;

    // This allows you to avoid: 'Bind: Address Already in Use' error
    int ret = setsockopt(server_sockd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    // Bind server address to socket that is open
    ret = bind(server_sockd, (struct sockaddr *) &server_addr, addrlen);

    // Listen for incoming connections
    ret = listen(server_sockd, SOMAXCONN); // 5 should be enough, if not use SOMAXCONN

    if (ret < 0) {
        return EXIT_FAILURE;
    }

    // Connecting with a client
    struct sockaddr client_addr;
    socklen_t client_addrlen;
    ssize_t client_socket;

    sem_init(&empty, 0, BUFFERSIZE);
    sem_init(&full, 0, 0);
    sem_init(&sem, 0, 1);


     // default number of threads if -N isn't specified
    if(threadFlag == -1){
        numThreads = 4;
    }

    pthread_t threads[numThreads];
    int thread_id[numThreads];

    for(int i = 0; i < numThreads; i++){
        thread_id[i] = i;
        pthread_create(&threads[i], NULL, executeWorker, &thread_id[i]);
    }

    // producer thread keeps updating array if there's a new request
    while(1){

        client_socket = accept(server_sockd, &client_addr, &client_addrlen);

        // printf("Producer got request id -> %d\n", client_sockd);
        sem_wait(&empty);                                   // wait until at least one slot is empty 
        // printf("Producer entering critical section\n");
        sem_wait(&sem);                                     // thread enters critical section
        array[in] = client_socket;
        in = (in + 1) % BUFFERSIZE;
        // printf("Producer adds to the array\n");
        sem_post(&sem);                                     // thread leaves critical section
        // printf("Procuer just left critical section\n");
        sem_post(&full);                                    // added an item to the array, so increment full
        // printf("Up to the thread now\n\n");
    }

    return 0;
}