//
//  uvsg_serial_data.h
//  Atari800
//
//  Created by Ari on 4/18/20.
//

#include <stdbool.h>
#include <stdio.h>

typedef struct UVSGSerialDataReceiver UVSGSerialDataReceiver;

UVSGSerialDataReceiver *UVSGSerialDataReceiverCreate(void);
void UVSGSerialDataReceiverFree(UVSGSerialDataReceiver *receiver);

void UVSGSerialDataReceiverStart(UVSGSerialDataReceiver *receiver, int port);
void UVSGSerialDataReceiverStop(UVSGSerialDataReceiver *receiver);
bool UVSGSerialDataReceiverIsStarted(UVSGSerialDataReceiver *receiver);

size_t UVSGSerialDataReceiverReceiveData(UVSGSerialDataReceiver *receiver, void **receivedData);
