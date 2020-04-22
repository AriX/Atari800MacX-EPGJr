//
//  uvsg_serial_data.c
//  Atari800
//
//  Created by Ari on 4/18/20.
//

#include "uvsg_serial_data.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <Ws2tcpip.h>
#else
#include <errno.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

// MARK: Platform-specific declarations

#ifdef _WIN32
typedef SOCKET UVSGSocket;
#define UVSG_SOCKET_INVALID INVALID_SOCKET
#else
typedef int UVSGSocket;
#define UVSG_SOCKET_INVALID -1
#endif

static int getUVSGSocketError(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

#ifdef _WIN32
#define EWOULDBLOCK WSAEWOULDBLOCK
#define EAGAIN EWOULDBLOCK
#else
#define SOCKADDR_INET struct sockaddr_storage
#endif

// MARK: Internal

#define SERIAL_TCP_BUFFER_LENGTH 1024

typedef enum {
    UVSGConnectionStatusStopped = 0,
    UVSGConnectionStatusError,
    UVSGConnectionStatusWaitingForConnection,
    UVSGConnectionStatusConnected
} UVSGConnectionStatus;

struct UVSGSerialDataReceiver {
    UVSGConnectionStatus connectionStatus;
    UVSGSocket tcpSocket;
    UVSGSocket tcpConnection;
    char buffer[SERIAL_TCP_BUFFER_LENGTH];
};

static void UVSGInitializeSocketSupport(void) {
    static bool initialized = false;
    if (initialized)
        return;
    
    // Perform Windows-specific initialization
#ifdef _WIN32
    static WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata))
        fprintf(stderr, "WSAStartup failed with error %d", WSAGetLastError());
#endif

    initialized = true;
}

static UVSGSocket UVSGCreateTCPSocket(int port) {
    UVSGInitializeSocketSupport();

    // Create socket
    UVSGSocket tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcpSocket == UVSG_SOCKET_INVALID) {
        fprintf(stderr, "uvsg_serial_data: TCP socket creation error: %d", getUVSGSocketError());
        return UVSG_SOCKET_INVALID;
    }
    
    // Set reuse address option on socket
    int socketOptionValue = 1;
    int result = setsockopt(tcpSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&socketOptionValue, sizeof(socketOptionValue));
    if (result < 0) {
        fprintf(stderr, "uvsg_serial_data: setsockopt(SO_REUSEADDR) failed: %d", getUVSGSocketError());
        close(tcpSocket);
        return UVSG_SOCKET_INVALID;
    }
    
    // Set socket to non-blocking
#ifdef _WIN32
    u_long mode = 1;  // 1 to enable non-blocking socket
    int status = ioctlsocket(tcpSocket, FIONBIO, &mode);
#else
    int status = fcntl(tcpSocket, F_SETFL, fcntl(tcpSocket, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (status != 0) {
        fprintf(stderr, "uvsg_serial_data: fcntl(O_NONBLOCK) failed: %d", getUVSGSocketError());
        close(tcpSocket);
        return UVSG_SOCKET_INVALID;
    }
    
    // Bind socket to a port
    struct sockaddr_in serverAddress = {0};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    result = bind(tcpSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    if (result < 0) {
        fprintf(stderr, "uvsg_serial_data: bind() failed: %d", getUVSGSocketError());
        close(tcpSocket);
        return UVSG_SOCKET_INVALID;
    }
    
    // Make socket ready to accept connections
    result = listen(tcpSocket, 1);
    if (result < 0) {
        fprintf(stderr, "uvsg_serial_data: listen() failed: %d\n", getUVSGSocketError());
        close(tcpSocket);
        return UVSG_SOCKET_INVALID;
    }

    return tcpSocket;
}

static void UVSGSerialDataReceiverAcceptConnection(UVSGSerialDataReceiver *receiver) {
    // Accept any waiting connection requests
    socklen_t clientAddressLength = sizeof(SOCKADDR_INET);
    char clientAddress[sizeof(SOCKADDR_INET)];
    UVSGSocket connection = accept(receiver->tcpSocket, (struct sockaddr *)clientAddress, &clientAddressLength);
    if (connection == -1) {
        int error = getUVSGSocketError();
        
        // No connections are waiting
        if (error == EWOULDBLOCK)
            return;
        
        fprintf(stderr, "uvsg_serial_data: accept() failed: %d\n", error);
        return;
    }
    
    receiver->connectionStatus = UVSGConnectionStatusConnected;
    receiver->tcpConnection = connection;
}

static size_t UVSGSerialDataReceiverReadFromConnection(UVSGSerialDataReceiver *receiver, void **receivedData) {
    int byteCount = recv(receiver->tcpConnection, receiver->buffer, SERIAL_TCP_BUFFER_LENGTH, 0);
    if (byteCount < 0) {
        int error = getUVSGSocketError();
        
        // No data is waiting
        if (error == EAGAIN)
            return 0;
        
        fprintf(stderr, "uvsg_serial_data: Error reading from socket: %d\n", error);
        return 0;
    }
    
    if (byteCount == 0) {
        // Client disconnected
        close(receiver->tcpConnection);
        receiver->connectionStatus = UVSGConnectionStatusWaitingForConnection;
        return 0;
    }
    
    *receivedData = receiver->buffer;
    return byteCount;
}


// MARK: Public interface

UVSGSerialDataReceiver *UVSGSerialDataReceiverCreate(void) {
    UVSGSerialDataReceiver *receiver = malloc(sizeof(UVSGSerialDataReceiver));
    receiver->connectionStatus = UVSGConnectionStatusStopped;
    return receiver;
}

void UVSGSerialDataReceiverStart(UVSGSerialDataReceiver *receiver, int port) {
    UVSGSocket socket = UVSGCreateTCPSocket(port);
    
    if (socket == UVSG_SOCKET_INVALID) {
        receiver->connectionStatus = UVSGConnectionStatusError;
        return;
    }
    
    receiver->connectionStatus = UVSGConnectionStatusWaitingForConnection;
    receiver->tcpSocket = socket;
}

void UVSGSerialDataReceiverStop(UVSGSerialDataReceiver *receiver) {
    switch (receiver->connectionStatus) {
        case UVSGConnectionStatusStopped:
        case UVSGConnectionStatusError:
            return;
        case UVSGConnectionStatusWaitingForConnection:
            close(receiver->tcpConnection);
        case UVSGConnectionStatusConnected:
            close(receiver->tcpSocket);
            receiver->connectionStatus = UVSGConnectionStatusStopped;
    }
}

void UVSGSerialDataReceiverFree(UVSGSerialDataReceiver *receiver) {
    UVSGSerialDataReceiverStop(receiver);
    free(receiver);
}

bool UVSGSerialDataReceiverIsStarted(UVSGSerialDataReceiver *receiver) {
    UVSGConnectionStatus connectionStatus = receiver->connectionStatus;
    return (connectionStatus == UVSGConnectionStatusWaitingForConnection || connectionStatus == UVSGConnectionStatusConnected);
}

size_t UVSGSerialDataReceiverReceiveData(UVSGSerialDataReceiver *receiver, void **receivedData) {
    switch (receiver->connectionStatus) {
        case UVSGConnectionStatusStopped:
        case UVSGConnectionStatusError:
        default:
            return 0;
        case UVSGConnectionStatusWaitingForConnection:
            UVSGSerialDataReceiverAcceptConnection(receiver);
            return 0;
        case UVSGConnectionStatusConnected:
            return UVSGSerialDataReceiverReadFromConnection(receiver, receivedData);
    }
}
