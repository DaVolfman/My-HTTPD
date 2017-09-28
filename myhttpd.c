/// @file	myhttpd.c
/// @author	Steven Clark (clark214@csusm.edu, davolfman@gmail.com)


#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

#define svcbuffersize 256

/// @brief Enumerated request methods for an HTTP request
enum htcommand{
	INVALID,
	GET,
	POST,
	HEAD
};

/// @brief	Gets a token from a buffer backed by an open file descriptor
/// @param	filedesc	A file descriptor open for reading, either a socket or file works
/// @param	buffer		A pointer to a character array in which data is stored after being read from a file
/// @param	bufflen		The maximum number of characters in the buffer.  last character used for '\0' delimeter
/// @param	bufferleft	A pointer to a pointer to the unprocessed portion of the buffer to extract a token from with strsep
/// @param	delim		An array of delimiter characters
/// @return	A pointer to the first token found in *bufferleft
char *filesep(int filedesc, char* buffer, size_t bufflen, char **bufferleft, const char * delim){
	char* fieldptr = NULL;	//the pointer to the token that will be returned
	int partlen = 0;		//If the buffer only contained a partial token, the length of that segment
	
	if(NULL == bufferleft)	//if the last token was already pulled, reset it
		*bufferleft = buffer;
	else
		fieldptr = strsep( bufferleft, delim);// in normal cases use strsep to get a token from unused buffer

	if(NULL == fieldptr){//if the last token was pulled previously
		bzero(buffer, bufflen);//wipe the buffer
		read(filedesc, buffer, bufflen-1);//refill the buffer
		*bufferleft = buffer;//reset bufferleft
		fieldptr = strsep(bufferleft, delim);//get a new field
	}else if(NULL == *bufferleft ){//if the token we just pulled was incomplete
		partlen = strlen(fieldptr);
		memmove(buffer, fieldptr, partlen);//move the partial to the front of the buffer
		bzero(buffer+partlen, bufflen - partlen);//wipe the rest of the buffer
		read(filedesc, buffer + partlen, (bufflen - 1) - partlen);//read more in after it
		*bufferleft = buffer;//reset the unused portion
		fieldptr = strsep(bufferleft, delim);//reread the (now hopefully complete) token
	}

	return fieldptr;
}

/// @brief report an HTTP error to a client
/// @param netfd File descriptor for the socket to the client
/// @param hterror The HTTP status code for the error
/// @note This will close the connection
void reporterror(int netfd, int hterror){
	char errorbuffer[256];
	char errdes[64];
	
	if(400 == hterror)
		strcpy(errdes, "Bad Request");
	if(405 == hterror)
		strcpy(errdes, "Method Not Allowed");
	if(404 == hterror)
		strcpy(errdes, "Not Found");
	
	sprintf(errorbuffer, "HTTP/1.0 %d %s\n\n", hterror, errdes);
	write(netfd, errorbuffer, strlen(errorbuffer));
	close(netfd);
}


/// @brief Process an HTTP request from an open socket, pthread format
/// @param param A pointer to an integer file descriptor of an open client HTTP scoket, cast as a void *
/// @return A void pointer, currently unused
void *svcthread(void *param){
	int sockfd = *(int *)param;	//socket file descriptor
	char buffer[svcbuffersize]; //buffer from the socket
	char *bufferleft;			//unused portion of that buffer (for strsep)
	char *field = NULL;			//points to a token pulled from the buffer of the socket

	int herror = 0;							// Current HTTP status code or other error, 0 during normal operation
	enum htcommand methodtype = INVALID;	// The Method of the HTTP request
	int httpver = 0;						// The HTTP version requested by the client.  Parsed and ignored for now
	char entity[svcbuffersize];				// Holds the Entity from the request
	char hostline[svcbuffersize];			// Holds the value of the Host header field of the request, if any, then ignored

	int emptycount = 0;				//the number of empty tokens read from the stream '\n' line endings will break this
	size_t bytesread = 0;			//The number of bytes read by the initial read of the socket
	char headerline[svcbuffersize];	//A string holding a line of header output during composition
	int filefd;						//file descriptor of the entity
	char filebuff[svcbuffersize];	//buffer for reading from or writing to the entity

	time_t timenowt = 0;	//will hold the system time as a time_t
	struct tm timenow;		//timenowt converted to struct tm
	struct stat filestat;	//the stat structure for the entity
	struct tm filetime;		//the last-modified time of the entity
	int fnotexists;			//error status of static the entity, nonzero when it doesn't exist

	char * scratchptr;
	size_t scratchsize, isize, scratchsize2;

	long int threadnum = pthread_self();

	//zero out buffers
	bzero(buffer, svcbuffersize);
	bufferleft = buffer;
	bzero(hostline, svcbuffersize);
	bzero(entity ,svcbuffersize);
	bzero(headerline ,svcbuffersize);
	bzero(filebuff ,svcbuffersize);

	//prime the socket buffer
	bytesread = read(sockfd, buffer, svcbuffersize-1);

	if(bytesread <= 0){//if unable to read from the socket print a local error message
		fprintf(stderr, "%ld : %d Initial read failed\n", threadnum, sockfd);
		close(sockfd);
		herror = -1;
	}

	if(0 == herror){//if all ok, process the request method
		field = filesep(sockfd, buffer, svcbuffersize, &bufferleft, " \r\n");
		if(NULL == field){
			herror = -2;
		}else if(strcmp(field, "GET") == 0){
			methodtype = GET;
		}else if(strcmp(field, "HEAD") == 0){
			methodtype = HEAD;
		}else if(strcmp(field, "POST") == 0){
			methodtype = POST;
		}else{
			methodtype = INVALID;
			herror = 405;
			reporterror(sockfd, herror);
		}
	}

	if(0 == herror){//If all still OK process the entity
		bzero(entity, svcbuffersize);
		field = filesep(sockfd, buffer, svcbuffersize, &bufferleft, " \r\n");
		if('\0' != field[0]){
			strcpy(entity, field);
		}else {
			herror = 400;
			reporterror(sockfd, herror);
		}

	}

	emptycount = 0;
	if(0 == herror){//if all ok, process the HTTP version
		field = filesep(sockfd, buffer, svcbuffersize, &bufferleft, " \r\n");
		/*fprintf(stderr, "%s\n", field);*/
		if('\0' == field[0]){ //assume 1.0 if not specified
			httpver = 10;
			emptycount = 1;
		}else if(strcmp(field, "HTTP/1.0") == 0){
			httpver = 10;
		}else if(strcmp(field, "HTTP/1.1") == 0){
			httpver = 11;
		}else {
			herror = 400;
			reporterror(sockfd, herror);
		}
	}

	if(0 == herror){//if all OK get header fields
		while(2 > emptycount){ //until there's an empty line
			field = filesep(sockfd, buffer, svcbuffersize, &bufferleft, " \r\n");//get a token from socket
			if(NULL == field){
				herror = -3;//if there's an error reading from the socket, exit
				break;
			}

			if('\0' == field[0]){
				++emptycount;//if there's an empty token increase the count of the run of empty tokens
			}else{
				emptycount = 0;//reset the count of empty tokens in  a row
				if(0 == strcmp(field, "Host:")){ //if this it the Host line
					field = filesep(sockfd, buffer, svcbuffersize, &bufferleft, " \r\n");//get the value of the line
					if('\0' == field[0]){ //if it's empty give an error to the client.
						herror = 400;
						reporterror(sockfd, herror);
						break;
					}else{//if it's not empty record it
						strncpy(hostline, field, svcbuffersize-1);
					}
				}
			}
		}
	}

	// if everything OK process the request
	if(0 == herror){
		//Convert the entity to a relative file path
		memmove(entity+1,entity, strlen(entity) + 1);
		entity[0] = '.';

		fnotexists = stat(entity, &filestat); //record if the file doesn't exist
		timenowt = time(NULL);
		gmtime_r(&timenowt, &timenow);//get the time

		if(fnotexists != 0 /*&& (GET == methodtype || HEAD == methodtype)*/){
			herror = 404;//If it doesn't exist return 404 to client
			reporterror(sockfd, herror);
		}else{
			//write the fixed portions of the header
			write(sockfd, "HTTP/1.0 200 OK\r\n", 17);
			strftime(headerline, svcbuffersize, "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &timenow);
			write(sockfd, headerline, strlen(headerline));
			write(sockfd, "Server: myhttpd/0.5 (Unix)\r\n", 28);
			strcpy(headerline, "Transfer-Encoding: identity\r\n");
			write(sockfd, headerline, strlen(headerline));
			strcpy(headerline, "Connection: close\r\n");
			write(sockfd, headerline, strlen(headerline));

			// if GET or HEAD send file info
			if( GET == methodtype || HEAD == methodtype){
				//get the mtime of the file and send it as the Last-Modified response header in HTTP date format
				gmtime_r(&(filestat.st_mtime),&filetime);
				strftime(headerline, svcbuffersize, "Last-Modified: %a, %d %b %Y %H:%M:%S GMT\r\n", &filetime);
				write(sockfd, headerline, strlen(headerline));

				//check the file extension
				scratchptr = entity + strlen(entity) - 5;
				if(strcmp(scratchptr, ".jpeg") == 0 || strcmp(scratchptr+1,".jpg") == 0){
					strcpy(headerline, "Content-Type: image/jpeg\r\n");//if it's a jpeg return the correct content type header
					write(sockfd, headerline, strlen(headerline));
				}

				//Send the length of the file as Content-Length
				sprintf(headerline, "Content-Length: %ld\r\n", filestat.st_size);
				write(sockfd, headerline, strlen(headerline));
			}
			write(sockfd, "\r\n", 2); //send the balnk line
			if( GET == methodtype){  //if GET
				isize = 0;
				bzero(filebuff, svcbuffersize);
				filefd = open(entity,O_RDONLY);
				while(isize < filestat.st_size){ //until the length of the file has been sent
					scratchsize = read(filefd, filebuff, svcbuffersize-1);//read bufferfulls
					scratchsize2 = write(sockfd, filebuff, scratchsize);//send those bufferfulls
					if( scratchsize != scratchsize2)
						fprintf(stderr, "Socket underflow, %ld read, %ld written\n", scratchsize, scratchsize2);//let us know if they don't match locally
					isize+=scratchsize;
				}
			}
			if( POST == methodtype ){//if POST
				filefd = open(entity, O_RDWR | O_APPEND);//append to the named file
				do{
					scratchsize = read(sockfd, filebuff, svcbuffersize);//read bufferfulls from the socket
					write(filefd, filebuff, scratchsize);//append them to the file
				}while( scratchsize > 0);//until an empty buffer was read or an error occured AKA the socket closed
			}
		}
	}

	if(herror == 0)
		close(sockfd);//close the socket if there was no error

	return NULL;
}

/// @brief Listen for HTTP requests on the specified port
/// @param argc the number of command line arguments
/// @param argv A pointer to an array of cstring pointers of the words of the command line
/// @return 0 for success
int main(int argc, char ** argv){
    int sockfd, clisockfd, port;
    socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    pthread_t  tid;

   	struct sockaddr_in sockinfo;
   	socklen_t sockinfolen = sizeof(sockinfo);
    int threadfail;

    if (argc < 2) {//If no port specified
    	port = 0;
    }else{
        port = atoi(argv[1]);
    }

    /* Open a TCP socket connection */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
       fprintf(stderr, "Error opening socket, errno = %d (%s) \n",
               errno, strerror(errno));
       return -1;
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "Error bind to socket, errno = %d (%s) \n",
                errno, strerror(errno));
        return -1;
    }

    /* Setup passive listening socket for client connections */
    listen(sockfd, 5);

    if(argc < 2){//if the port was not specified and then auto-allocated
    	getsockname(sockfd, (struct sockaddr *) &sockinfo, &sockinfolen);
    	fprintf(stderr, "Listening on port %d\n", ntohs(sockinfo.sin_port));
    }   //report the new port number to the user

    /* Wait for incoming socket connection requests */
    while (1) {
        clilen = sizeof(cli_addr);
        clisockfd = accept(sockfd,
                           (struct sockaddr *) &cli_addr,
                           &clilen);

        if (clisockfd < 0) {
            fprintf(stderr, "Error accepting socket connection request, errno = %d (%s) \n",
                    errno, strerror(errno));
            break;
        }

        /* Create thread for client requests/responses */
        threadfail = pthread_create(&tid, NULL, (void *)&svcthread, (void *)&clisockfd);
        if( threadfail == 0){//if that worked
        	pthread_detach(tid);//detach the thread so we don't need to join it to clean up.
        }

    }

    close(sockfd);

    return 0;
}
