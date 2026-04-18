#Makefile for client and server

CC = gcc

CLIENT_SRCS = client.c
SERVER_SRCS = server.c
CFLAGS = -g -Wall -I/opt/homebrew/include -I/usr/local/include
LIBS = -L/opt/homebrew/lib -lcjson -lssl -lcrypto

all: client server
client: $(CLIENT_SRCS)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_SRCS) $(LIBS)
server: $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS) $(LIBS)

clean: 
	rm -f client server