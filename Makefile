CC = g++
CFLAG = -O2 -lpthread
ifeq ($(MODE),rd)
    CFLAGS -= O2
    CFLAGS += -g3 -gdwarf-4 -ggdb3 -DDEBUG
endif

all: myftpclient myftpserver mygbn.o myftpclient.o myftpserver.o

mygbn.o : mygbn.c mygbn.h
	$(CC) -o $@ -c $< $(CFLAG)

myftpserver.o: myftpserver.c mygbn.h
	$(CC) -o $@ -c $< $(CFLAG)

myftpserver: myftpserver.o mygbn.o
	$(CC) -o $@ $^ $(CFLAG)

myftpclient.o: myftpclient.c mygbn.h
	$(CC) -o $@ -c $< $(CFLAG)

myftpclient: myftpclient.o mygbn.o 
	$(CC) -o $@ $^ $(CFLAG)

clean:
	rm -f myftpserver myftpclient mygbn.o myftpclient.o myftpserver.o
