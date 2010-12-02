/** ModularEEG acqusition tool to stream data to a FieldTrip buffer,
	and write data to one or multiple GDF files (if you ever reach the size limit...).
	(C) 2010 S. Klanke
*/
#include <SignalConfiguration.h>
#include <stdio.h>
#include <FtBuffer.h>
#include <socketserver.h>
#include <pthread.h>
#include <LocalPipe.h>
#include <GdfWriter.h>
#include <ConsoleInput.h>
#include <serial.h>

#define NUM_HW_CHAN 6
#define FSAMPLE     256
#define PACKET_LEN  17

int64_t maxFileSize = 1024*1024*1024; // 1GB for testing
// lock-free FIFO for saving thread
int16_t *rbInt;
int rbIntSize, rbIntChans;
int rbIntWritePos;
int rbIntReadPos;
// pipe or socketpair for inter-thread communication
LocalPipe pipe;
// GDF writing object
GDF_Writer *gdfWriter = NULL;
// Name of file to write to
char baseFilename[1024];
char curFilename[1024];
int fileCounter = 0;


bool writeHeader(int ftSocket, float fSample, const ChannelSelection &streamSel) {
	FtBufferRequest req;
	FtBufferResponse resp;
	char *chunk_data;
	int N=0,P;

	for (int n=0;n<streamSel.getSize();n++) {
		int Ln = strlen(streamSel.getLabel(n))+1;
		N+=Ln;
	}
	chunk_data = new char[N];
	
	P=0;
	for (int n=0;n<streamSel.getSize();n++) {
		int Ln = strlen(streamSel.getLabel(n))+1;
		memcpy(chunk_data + P, streamSel.getLabel(n), Ln);
		P+=Ln;
	}

	req.prepPutHeader(streamSel.getSize(), DATATYPE_FLOAT32, fSample);
	req.prepPutHeaderAddChunk(FT_CHUNK_CHANNEL_NAMES, N, chunk_data);
	
	delete[] chunk_data;
	
	int err = clientrequest(ftSocket, req.out(), resp.in());
	if (err || !resp.checkPut()) {
		fprintf(stderr, "Could not write header to FieldTrip buffer\n.");
		return false;
	}
	return true;
}

void *savingThreadFunction(void *arg) {
	int writePtr,n;
	int64_t fileSize = 256*(1+rbIntChans);

	while (1) {
		int newSamplesA, newSamplesB;
		int16_t *rbIntPtr;
		int64_t newSize;
		
		n = pipe.read(sizeof(int), &writePtr);
		if (n!=sizeof(int)) {
			// this should never happen for blocking sockets/pipes
			fprintf(stderr, "Unexpected error in pipe communication\n");
			break;
		}
		if (writePtr < 0) {
			printf("\nSaving thread received %i - exiting...\n", writePtr);
			break;
		}
		
		rbIntPtr = rbInt + rbIntReadPos*rbIntChans;
		if (writePtr > rbIntReadPos) {
			newSamplesA = writePtr - rbIntReadPos;
			newSamplesB = 0;
		} else {
			newSamplesA = rbIntSize - rbIntReadPos;
			newSamplesB = writePtr;
		}
		
		newSize = fileSize + (newSamplesA+newSamplesB) * rbIntChans * sizeof(int32_t);
		if (newSize > maxFileSize) {
			gdfWriter->close();
			
			fileCounter++;
			
			snprintf(curFilename, sizeof(curFilename), "%s_%i.gdf", baseFilename, fileCounter);
			if (!gdfWriter->createAndWriteHeader(curFilename)) {
				fprintf(stderr, "Error: could not create GDF file %s\n", curFilename);
				break;
			}
			newSize = 256*(1+rbIntChans) + (newSamplesA+newSamplesB) * rbIntChans * sizeof(int16_t);
		}
		
		gdfWriter->addSamples(newSamplesA, rbIntPtr);
		// mark as empty
		for (int i=0;i<newSamplesA;i++) {
			rbIntPtr[i*rbIntChans] = -1;
		}
		if (newSamplesB > 0) {
			gdfWriter->addSamples(newSamplesB, rbInt);
			for (int i=0;i<newSamplesB;i++) {
				rbInt[i*rbIntChans] = -1;
			}
		}
		rbIntReadPos = writePtr;
		fileSize = newSize;
	}
	return NULL;
}


int main(int argc, char *argv[]) {
	int port, ftSocket;
	int sampleCounter = 0;
	char hostname[256];
	FtBufferResponse resp;
	FtEventList   eventChain;
	FtSampleBlock sampleBlock(DATATYPE_FLOAT32);
	SignalConfiguration signalConf;
	int16_t switchState = 0;
	ft_buffer_server_t *ftServer = NULL;
	pthread_t savingThread;
	ConsoleInput ConIn;
	SerialPort SP;
	unsigned char serialBuffer[1024];
	int16_t sampleData[NUM_HW_CHAN * FSAMPLE]; // holds up to 1 seconds of data
	int16_t switchData[FSAMPLE]; // again, 1 second of data (switches)
	int leftOverBytes = 0;
	int nStream, nSave;
	
	if (argc<4) {
		printf("Usage: modeeg2ft <device> <config-file> <gdf-file> [hostname=localhost [port=1972]]\n");
		return 0;
	}
	
	int err = signalConf.parseFile(argv[2]);
	if (err == -1) {
		fprintf(stderr, "Could not read configuration file %s\n", argv[1]);
		return 1;
	}
	if (err > 0) {
		fprintf(stderr, "Encountered %i errors in configuration file - aborting\n", err);
		return 1;
	}
	
	// enable for debugging if you like
	if (0) {
		const ChannelSelection& streamSel = signalConf.getStreamingSelection();
		const ChannelSelection& saveSel = signalConf.getSavingSelection();
		
		printf("Selected for streaming:\n");
		for (int i=0;i<streamSel.getSize();i++) {
			printf("%3i: %s\n", streamSel.getIndex(i)+1, streamSel.getLabel(i));
		}
		printf("Selected for saving:\n");
		for (int i=0;i<saveSel.getSize();i++) {
			printf("%3i: %s\n", saveSel.getIndex(i)+1, saveSel.getLabel(i));
		}
	}
	
	strcpy(baseFilename, argv[3]);
	char *lastDotPos = strrchr(baseFilename, '.');
	// cut off .gdf suffix, if given
	if (lastDotPos != NULL) {
		if (!strcasecmp(lastDotPos, ".gdf")) {
			*lastDotPos = 0;
		}
	}		
	strcpy(curFilename, baseFilename);
	strcat(curFilename, ".gdf");
	
	if (argc>4) {
		strncpy(hostname, argv[4], sizeof(hostname));
	} else {
		strcpy(hostname, "localhost");
	}
	
	if (argc>5) {
		port = atoi(argv[5]);
	} else {
		port = 1972;
	}
	
	if (strcmp(hostname, "-")) {
		ftSocket = open_connection(hostname, port);
		if (ftSocket < 0) {
			fprintf(stderr, "Could not connect to FieldTrip buffer on %s:%i\n", hostname, port);
			return 1;
		}
	} else {
		ftServer = ft_start_buffer_server(port, NULL, NULL, NULL);
		if (ftServer == NULL) {
			fprintf(stderr, "Could not spawn TCP server on port %i\n", port);
			return 1;
		}
		ftSocket = 0;
	}
	
	if (NUM_HW_CHAN < signalConf.getMaxSavingChannel() || NUM_HW_CHAN < signalConf.getMaxStreamingChannel() ) {
		fprintf(stderr, "Configuration file specifies channel numbers beyond hardware limits.\n");
		return 1;
	}
	
	const ChannelSelection& streamSel = signalConf.getStreamingSelection();
	const ChannelSelection& saveSel = signalConf.getSavingSelection();
	
	printf("Selected %i channels for streaming, %i channels for saving to GDF.\n", streamSel.getSize(), saveSel.getSize());
		
	if (saveSel.getSize() > 0) {
		gdfWriter = new GDF_Writer(1+saveSel.getSize(), FSAMPLE, GDF_INT16);
		gdfWriter->setLabel(0, "Switches");
		for (int i=0;i<saveSel.getSize();i++) {
			gdfWriter->setLabel(1+i, saveSel.getLabel(i));
			//gdfWriter->setPhysicalLimits(1+i, -0.262144, 0.26214399987792969);
			//gdfWriter->setPhysDimCode(1+i, GDF_VOLT);
		}
		if (!gdfWriter->createAndWriteHeader(curFilename)) {
			fprintf(stderr, "Could not open GDF file %s for writing\n", curFilename);
			return 1;
		}
	}
	
	if (!writeHeader(ftSocket, FSAMPLE, streamSel)) {
		fprintf(stderr, "Could not write header to FT buffer\n");
		return 1;
	}
	
	rbIntSize = 2*FSAMPLE;	// enough space for 2 seconds of data
	rbIntChans = 1+saveSel.getSize();
	rbInt = new int16_t[rbIntChans*rbIntSize];
	rbIntWritePos = 0;
	rbIntReadPos = 0;
	for (int i=0;i<rbIntSize;i++) rbInt[i*rbIntChans]=-1;
	
	if (!serialOpenByName(&SP, argv[1])) {
		fprintf(stderr, "Could not open serial port %s\n", argv[1]);
		return 1;
	}
	
	// last parameter is timeout in 1/10 of a second
	if (!serialSetParameters(&SP, 57600, 8, 0, 1, 1)) {
		fprintf(stderr, "Could not modify serial port parameters\n");
		return 1;
	}
	
	if (pthread_create(&savingThread, NULL, savingThreadFunction, NULL)) {
		fprintf(stderr, "Could not spawn GDF saving thread.\n");
		return 1;
	}
			
	printf("\nPress <Esc> to quit\n\n");
	
	nStream = streamSel.getSize();
	nSave   = saveSel.getSize();
	
	// read bytes until we get 0xA5,0x5A,...
	for (int iter = 0;iter < 200; iter++) {
		// with a 0.1s timeout, this will run 20 seconds max
		unsigned char byte;
		int nr;
		
		nr = serialRead(&SP, 1, &byte);
		if (nr<0) {
			fprintf(stderr, "Error when reading from serial port - exiting\n");
			return 1;
		} 
		if (nr==0) {
			printf(".");
			continue;
		}
		
		if (byte == 0xA5) {
			serialBuffer[0] = byte;
			leftOverBytes = 1;
		} else if (leftOverBytes == 1 && byte == 0x5A) {
			serialBuffer[1] = byte;
			leftOverBytes = 2;
			break; // success !
		} else {
			leftOverBytes = 0;
		}
	}
	if (leftOverBytes != 2) {
		fprintf(stderr, "Could not read synchronisation bytes from ModularEEG\n");
		goto cleanup;
	}
	
	printf("Got synchronization bytes - starting acquisition\n");
	
	//while (keepRunning) {
	while (1) {
		int numRead, numTotal, numBlocks;
		
		if (ConIn.checkKey()) {
			int c = ConIn.getKey();
			if (c==27) break; // quit
		}
		
		//numRead = serialRead(&SP, sizeof(serialBuffer) - leftOverBytes, serialBuffer + leftOverBytes);
		numRead = serialRead(&SP, PACKET_LEN - leftOverBytes, serialBuffer + leftOverBytes);
		if (numRead < 0) {
			fprintf(stderr, "Error when reading from serial port - exiting\n");
			break;
		}
		if (numRead == 0) continue;
		
		eventChain.clear();
		
		numTotal = leftOverBytes + numRead;
		numBlocks = numTotal / PACKET_LEN;
		
		if (numBlocks > FSAMPLE) {
			fprintf(stderr, "Received too much data from serial port - exiting.\n");
			break;
		}
		
		if (numBlocks == 0) continue;
		
		// first decode into switchData + sampleData arrays
		for (int j=0;j<numBlocks;j++) {
			int soff = j*PACKET_LEN;
			int doff = j*NUM_HW_CHAN;
			
			if (serialBuffer[soff] != 0xA5 || serialBuffer[soff+1] != 0x5A) {
				fprintf(stderr, "ModularEEG out of sync in sample %i - exiting.\n", sampleCounter + j);
				break;
			}
				
			for (int i=0;i<NUM_HW_CHAN;i++) {
				sampleData[i+doff] = serialBuffer[soff+4+2*i]*256 + serialBuffer[soff+5+2*i];
			}
		
			switchData[j] = serialBuffer[soff+16];
			if (switchData[j] != switchState) {
				switchState = switchData[j];
				if (switchState!=0) {
					eventChain.add(sampleCounter + j, "Switch", switchState);
				}
			}
		}
	
		// if saving is enabled, append to GDF file
		if (nSave > 0) {
			for (int j=0;j<numBlocks;j++) {
				int doff = rbIntWritePos * rbIntChans;

				if (rbInt[doff] != -1) {
					fprintf(stderr, "Error: saving thread does not keep up with load\n");
					break;
				}
			
				rbInt[doff++] = switchData[j];
				for (int i=0;i<nSave;i++) {
					rbInt[doff++] = sampleData[streamSel.getIndex(i) + j*NUM_HW_CHAN];
				}
				if (++rbIntWritePos == rbIntSize) rbIntWritePos = 0;
			}
			pipe.write(sizeof(int), static_cast<void *>(&rbIntWritePos)); 
		}
	
		if (nStream > 0) {
			float *dest = (float *) sampleBlock.getMatrix(nStream, numBlocks);
			if (dest==NULL) {
				fprintf(stderr, "Out of memory\n");
				break;
			}
			for (int j=0;j<numBlocks;j++) {
				for (int i=0;i<nStream;i++) {
					dest[i + j*nStream] = sampleData[streamSel.getIndex(i) + j*NUM_HW_CHAN];
				}
			}
			int err = clientrequest(ftSocket, sampleBlock.asRequest(), resp.in());
			if (err || !resp.checkPut()) {
				fprintf(stderr, "Could not write samples to FieldTrip buffer\n.");
			}
		}

		if (ftSocket != -1 && eventChain.count() > 0) {
			int err = clientrequest(ftSocket, eventChain.asRequest(), resp.in());
			if (err || !resp.checkPut()) {
				fprintf(stderr, "Could not write events to FieldTrip buffer\n.");
			}
		}

		sampleCounter += numBlocks;
		leftOverBytes = numTotal - numBlocks * PACKET_LEN;
		
		if (leftOverBytes > 0) {
			memcpy(serialBuffer, serialBuffer + numBlocks*PACKET_LEN, leftOverBytes);
		}
	}

cleanup:	
	int quitValue = -1;
	pipe.write(sizeof(int), &quitValue);
	
	serialClose(&SP);
	if (ftSocket > 0) close_connection(ftSocket);
	if (ftServer != NULL) ft_stop_buffer_server(ftServer);
	if (gdfWriter) {
		gdfWriter->close();
		delete gdfWriter;
	}
	
	return 0;
}
