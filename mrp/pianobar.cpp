/*
 *  pianobar.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 2/10/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#include "pianobar.h"

// Open and initialize the audio stream containing real-time (modified) Piano Bar input data.
// This will always come in the form of a 10.7kHz, 16-bit, 10-channel data stream.  
// historyInSeconds defines the amount of key history to save for each key.  This controls the
// size of the history buffers, which will be larger for the black keys (sampled at 1783Hz) than the
// white keys (594Hz).
// Return true on success.

// **** NOTE ****

// To work properly, this requires a modified portaudio library which converts float data to int16 by
// multiplying by 32768 (not 32767).  This is the only way (on Mac, at least) that the 16-bit input data
// comes out unchanged from the way it was transmitted via USB.

bool PianoBarController::open(PaDeviceIndex inputDeviceNum, int bufferSize, float historyInSeconds)
{
	const PaDeviceInfo *deviceInfo;
	PaStreamParameters inputParameters;
	int i, keyHistoryLengthBlack, keyHistoryLengthWhite;
	PaError err;
	
	close();	// Close any existing stream
	
	deviceInfo = Pa_GetDeviceInfo(inputDeviceNum);
	if(deviceInfo == NULL)
	{
		cerr << "Unknown PianoBar device " << inputDeviceNum << ".  Try -l for list of devices.\n";
		return false;
	}		
	
	cout << "PianoBar Input Device:  " << deviceInfo->name << endl;	
		
	if(deviceInfo->maxInputChannels < 10)
	{
		cerr << "Invalid PianoBar device.  Device should support at least 10 channels.\n";
		return false;
	}

	inputParameters.device = inputDeviceNum;
	inputParameters.channelCount = 10;
	inputParameters.sampleFormat = paInt16;				// Use 16-bit int for input
	inputParameters.suggestedLatency = Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
	
	// If we're on Mac, make sure we set the hardware to the actual sample rate and don't rely on SRC
	if(deviceInfo->hostApi == paCoreAudio || deviceInfo->hostApi == 0)	// kludge for portaudio bug?
	{
		PaMacCoreStreamInfo macInfo;
		
		PaMacCore_SetupStreamInfo(&macInfo, paMacCoreChangeDeviceParameters | paMacCoreFailIfConversionRequired);
		inputParameters.hostApiSpecificStreamInfo = &macInfo;
	}
	else
		inputParameters.hostApiSpecificStreamInfo = NULL;

	// Check whether these parameters will work with the given sample rate
	err = Pa_IsFormatSupported(&inputParameters, NULL, PIANO_BAR_SAMPLE_RATE);
	if(err != paFormatIsSupported)
	{
		cerr << "Invalid PianoBar device.  Device should support 10.7kHz sample rate at 10 channels.\n";
		return false;
	}

	// Open the stream, passing our own (static) callback function
	// No dithering, no clipping: just the straight digital data!
	err = Pa_OpenStream(&inputStream_,
						&inputParameters,
						NULL,
						PIANO_BAR_SAMPLE_RATE,
						bufferSize,
						paDitherOff | paClipOff,
						staticAudioCallback,
						this);
	if(err != paNoError)
	{
		cerr << "Error opening PianoBar stream: " << Pa_GetErrorText(err) << endl;
		inputStream_ = NULL;
		return false;
	}

	// Update the global variables
	isInitialized_ = true;
	isRunning_ = false;								// not until we call start()
	calibrationStatus_ = kPianoBarNotCalibrated;
	bufferSize_ = bufferSize;
	currentTimeStamp_ = lastStateUpdate_ = lastStateMessage_ = 0;
	
	keyHistoryLengthWhite = (int)ceilf(historyInSeconds * (float)PIANO_BAR_SAMPLE_RATE / 18.0);
	keyHistoryLengthBlack = (int)ceilf(historyInSeconds * (float)PIANO_BAR_SAMPLE_RATE / 6.0);
	for(i = 0; i < 88; i++)
	{
		if(kPianoBarKeyColor[i] == K_B)
		{
			keyHistory_[i] = new int[keyHistoryLengthBlack];
			keyHistoryTimestamps_[i] = new pb_timestamp[keyHistoryLengthBlack];
			keyHistoryLength_[i] = keyHistoryLengthBlack;
			bzero(keyHistory_[i], keyHistoryLengthBlack*sizeof(int));
		}
		else
		{
			keyHistory_[i] = new int[keyHistoryLengthWhite];
			keyHistoryTimestamps_[i] = new pb_timestamp[keyHistoryLengthWhite];
			keyHistoryLength_[i] = keyHistoryLengthWhite;
			bzero(keyHistory_[i], keyHistoryLengthWhite*sizeof(int));
		}
		keyHistoryPosition_[i] = 0;
		keyIdleThreshold_[i] = 200;		
		
		debugLastPrintTimestamp_[i] = 0;
	}
	
	resetKeyStates();
	
	// Set various parameters controlling motion between states
	
	keyPressPositionThreshold_ = 3400; /*3072*/
	keyPressVelocityThreshold_ = (int)((float)VELOCITY_SCALER*64.0);
	keyPretouchIdleVelocity_ = (int)((float)VELOCITY_SCALER*2.0);
	keyDownVelocityThreshold_ = (int)((float)VELOCITY_SCALER*64.0);
	keyReleasePositionThreshold_ = 3072; // probably this should be the same as press position threshold
	keyIdleBounceTime_ = secondsToFrames(0.3);	
	keyDownBounceTime_ = secondsToFrames(0.05);
	keyAftertouchThreshold_ = 15;
	keyStuckPositionThreshold_ = 5;
	keyTapAccelerationThreshold_ = 5000;
	keyPreVibratoVelocity_ = (int)((float)VELOCITY_SCALER*7.0);
	keyPreVibratoTimeout_ = secondsToFrames(0.3);
	keyPreVibratoFrameSpacing_ = secondsToFrames(0.06);
	keyPretouchHoldoffVelocity_ = (int)((float)VELOCITY_SCALER*64.0);
	keyDownHoldoffVelocityWhite_ = (int)((float)VELOCITY_SCALER*48.0);
	keyDownHoldoffVelocityBlack_ = (int)((float)VELOCITY_SCALER*384.0);
	keyDownHoldoffTime_ = secondsToFrames(0.05);
	
	return true;
}

// Tell the currently open stream to begin capturing data.  Returns true on success.

bool PianoBarController::start()
{
	PaError err;
	
	if(!isInitialized_ || inputStream_ == NULL || isRunning_)
		return false;
	
	err = Pa_IsStreamActive(inputStream_);
	if(err > 0)
		return true;	// Stream is already running... nothing to do here ("success"?)
	else if(err < 0)
	{
		cerr << "Error in PianoBarController::start(): " << Pa_GetErrorText(err) << endl;
		return false;
	}
	
	// Reset this every time in case a key was stuck before
	for(int i = 0; i < 88; i++)
	{
		keyIdleThreshold_[i] = 200;
	}			
	
	err = Pa_StartStream(inputStream_);
	
	if(err != paNoError)
	{
		cerr << "Error in PianoBarController::start(): " << Pa_GetErrorText(err) << endl;
		return false;
	}
	
	isRunning_ = true;
	return true;
}

// Stop a currently running stream.  Returns true on success.

bool PianoBarController::stop()
{
	PaError err;
	
	if(!isInitialized_ || inputStream_ == NULL)
		return false;	
	
	err = Pa_IsStreamActive(inputStream_);
	if(err > 0)	// Stream is running
	{
		err = Pa_StopStream(inputStream_);
		isRunning_ = false;
		
		if(err != paNoError)
		{
			cerr << "Error in PianoBarController::stop(): " << Pa_GetErrorText(err) << endl;
			return false;		
		}
	}	
	
	return true;
}

// Close any currently active audio stream.  This is assumed to succeed (nothing we can do otherwise).

void PianoBarController::close()
{
	PaError err;
	
	if(!isInitialized_ || inputStream_ == NULL)
		return;
	
	stop();		// Stop the stream first
	
	err = Pa_CloseStream(inputStream_);
	if(err != paNoError)
		cerr << "Warning: PianoBarController::close() failed: " << Pa_GetErrorText(err) << endl;
	
	inputStream_ = NULL;
	isInitialized_ = false;		// No longer initialized without a stream open
	for(int i = 0; i < 88; i++)
	{
		if(keyHistory_[i] != NULL)
		{
			delete keyHistory_[i];	// Free up key history buffers
			delete keyHistoryTimestamps_[i];
			keyHistory_[i] = NULL;
			keyHistoryTimestamps_[i] = NULL;
		}
		keyHistoryLength_[i] = 0;
	}
}

bool PianoBarController::startCalibration(vector<int> &keysToCalibrate, bool quiescentOnly)
{
	int i, j, k;
	
	if(!isRunning_)
		return false;

	// This tells the audio capture callback to start saving data into the calibration buffers
	pthread_mutex_lock(&audioMutex_);
	
	calibrationStatus_ = kPianoBarInCalibration;	
	
	// Calibration gesture involves pressing each key lightly, then exerting heavy pressure.  For black keys,
	// light level (i.e. input value) will go up when this happens; for white keys, it will go down.  We assume
	// the start of calibration represents the quiescent values (average over several data points).  Then watch
	// for any big changes (again using heavy averaging).
	
	// Completion of key press will be marked by velocity going to zero and staying there.  Capture the first N
	// data points following this event as "light" press, then the max/min value as "heavy" press.
	
	calibrationHistoryLength_ = 64;			// Picked arbitrarily...
	calibrationSamples_ = 0;
	
	if(keysToCalibrate.size() > 0)
		keysToCalibrate_ = new vector<int>(keysToCalibrate);
	else
		keysToCalibrate_ = NULL;
	
	for(i = 0; i < 88; i++)
	{
		for(j = 0; j < 4; j++)
		{
			calibrationHistory_[i][j] = new short[calibrationHistoryLength_];
			
			for(k = 0; k < calibrationHistoryLength_; k++)
				calibrationHistory_[i][j][k] = UNCALIBRATED;
			
			if(keysToCalibrate_ != NULL)
			{
				bool foundMatch = false;
				
				for(k = 0; k < keysToCalibrate_->size(); k++)
				{
					if((*keysToCalibrate_)[k] == i + 21)
					{
						foundMatch = true;
						break;
					}
				}
				
				if(!foundMatch)
					continue;
			}
			
			calibrationQuiescent_[i][j] = UNCALIBRATED;
			
			if(!quiescentOnly)
			{
				calibrationLightPress_[i][j] = calibrationHeavyPress_[i][j] = UNCALIBRATED;
				calibrationOkToCalibrateLight_[i][j] = true;
			}
		}
	}
	
	pthread_mutex_unlock(&audioMutex_);

	// Let run for a short time to fill the history buffers, then collect quiescent values for each key
	while(calibrationSamples_ < 18*64)	// 18 samples per cycle, 64 cycles fills all buffers
		usleep(100);
	
	// Now (with mutex locked to prevent data changing) set the quiescent values for each key to a running
	// average of the last several samples
	pthread_mutex_lock(&audioMutex_);
	
	for(i = 0; i < 88; i++)
	{
		if(keysToCalibrate_ != NULL)
		{
			bool foundMatch = false;
			
			for(k = 0; k < keysToCalibrate_->size(); k++)
			{
				if((*keysToCalibrate_)[k] == i + 21)
				{
					foundMatch = true;
					break;
				}
			}
			
			if(!foundMatch)
				continue;
		}
		
		for(j = 0; j < 4; j++)
		{
			// At this point, each history buffer will either have filled up with real data, or be entirely empty
			// (for unused cycle values, e.g. cycle 4 on many keys).  In the latter case, we should get an averaged
			// value of UNCALIBRATED which is what we want.
			
			calibrationQuiescent_[i][j] = (short)calibrationRunningAverage(i, j, 0, 32);
		}
	}
	
	if(quiescentOnly)
	{
		calibrationStatus_ = kPianoBarCalibrated;	// Don't pick up the other values in this case
		
		if(keysToCalibrate_ != NULL)
			delete keysToCalibrate_;
		
		// When finished, free the calibration buffers-- we use a combined buffering system for regular usage
		
		for(i = 0; i < 88; i++)
		{
			for(j = 0; j < 4; j++)
			{
				delete calibrationHistory_[i][j];
			}
		}	
		
		cleanUpCalibrationValues();
	}
	
	pthread_mutex_unlock(&audioMutex_);
	
	if(quiescentOnly)
		return true;
	
#ifdef DEBUG_CALIBRATION		// Print calibration values
	for(i = 0; i < 88; i++)
		printf("Note %d: quiescent = (%hd, %hd, %hd, %hd)\n", i + 21,
			   calibrationQuiescent_[i][0], calibrationQuiescent_[i][1],
			   calibrationQuiescent_[i][2], calibrationQuiescent_[i][3]);
#endif	
	
	return true;
}

void PianoBarController::stopCalibration()
{
	int i, j;
	bool calibrationHadErrors = false;
	
	pthread_mutex_lock(&audioMutex_);
	calibrationStatus_ = kPianoBarCalibrated;
	
	if(keysToCalibrate_ != NULL)
		delete keysToCalibrate_;

	// When finished, free the calibration buffers-- we use a combined buffering system for regular usage
	
	for(i = 0; i < 88; i++)
	{
		for(j = 0; j < 4; j++)
		{
			delete calibrationHistory_[i][j];
		}
	}	
	
	calibrationHadErrors = cleanUpCalibrationValues();
	
	pthread_mutex_unlock(&audioMutex_);
	
	if(calibrationHadErrors)
		cout << "\nOne or more keys failed to calibrate.  Please run calibration again.\n";
}

bool PianoBarController::saveCalibrationToFile(string& filename)
{
	int i, j;
	
	if(calibrationStatus_ != kPianoBarCalibrated)
		return false;
	
	try
	{
		// Save the calibration to a text file
		// Format: "[note#] [sequence] [quiescent] [light] [heavy]"
		
		ofstream outputFile;
		
		outputFile.open(filename.c_str(), ios::out);
		for(i = 0; i < 88; i++)
		{
			for(j = 0; j < 4; j++)
			{
				outputFile << i << " " << j << " " << calibrationQuiescent_[i][j] << " ";
				outputFile << calibrationLightPress_[i][j] << " ";
				outputFile << calibrationHeavyPress_[i][j] << endl;
			}
		}
		outputFile.close();
	}
	catch(...)
	{
		return false;
	}
	
	return true;
}

// Private helper function called from both stopCalibration() and loadCalibrationFromFile() which
// sanity-checks the resulting data and flags any problems

bool PianoBarController::cleanUpCalibrationValues()
{
	bool calibrationHadErrors = false;
	int i;
	
#ifdef DEBUG_CALIBRATION		// Print calibration values
	for(i = 0; i < 88; i++)
		printf("Note %d: quiescent = (%hd, %hd, %hd, %hd), light = (%hd, %hd, %hd, %hd), heavy = (%hd, %hd, %hd, %hd)\n", i + 21,
			   calibrationQuiescent_[i][0], calibrationQuiescent_[i][1],
			   calibrationQuiescent_[i][2], calibrationQuiescent_[i][3],
			   calibrationLightPress_[i][0], calibrationLightPress_[i][1],
			   calibrationLightPress_[i][2], calibrationLightPress_[i][3],
			   calibrationHeavyPress_[i][0], calibrationHeavyPress_[i][1],
			   calibrationHeavyPress_[i][2], calibrationHeavyPress_[i][3]); 	
#endif
	
	resetKeyStates();
	
	// Check that we got decent values for cycle 0 for white keys and cycles 0-2 for black keys
	// Also print warnings if we can't read key pressure (this may depend on the piano)
	
	for(i = 0; i < 88; i++)
	{
		if(kPianoBarKeyColor[i] == K_W)	// white keys
		{
			if(calibrationQuiescent_[i][0] == UNCALIBRATED ||
			   calibrationLightPress_[i][0] == UNCALIBRATED ||
			   calibrationHeavyPress_[i][0] == UNCALIBRATED)
			{
				cout << "ERROR: Key " << i + 21 << " did not properly calibrate.\n";
				changeKeyState(i, kKeyStateDisabled);
				calibrationHadErrors = true;
			}
			
			// Prevent divide-by-0 errors later!
			if(calibrationLightPress_[i][0] == calibrationQuiescent_[i][0])
				calibrationLightPress_[i][0]--;			
			if(calibrationLightPress_[i][1] == calibrationQuiescent_[i][1])
				calibrationLightPress_[i][1]--;	
			if(calibrationLightPress_[i][2] == calibrationQuiescent_[i][2])
				calibrationLightPress_[i][2]--;	
			if(calibrationLightPress_[i][3] == calibrationQuiescent_[i][3])
				calibrationLightPress_[i][3]--;	
			
			// Reading key pressure requires a minimum differential between light and heavy key presses
			// On white keys, the heavy presses have the smaller value
			if(calibrationLightPress_[i][0] - calibrationHeavyPress_[i][0] > MIN_KEY_PRESSURE_DIFF)
				calibrationCanReadKeyPressure_[i] = true;
			else
			{
				calibrationCanReadKeyPressure_[i] = false;
				cout << "Warning: Can't read key pressure on [white] key " << i + 21 << endl;		
			}
		}
		else							// black keys
		{
			if(calibrationQuiescent_[i][0] == UNCALIBRATED ||
			   calibrationLightPress_[i][0] == UNCALIBRATED ||
			   calibrationHeavyPress_[i][0] == UNCALIBRATED ||
			   calibrationQuiescent_[i][1] == UNCALIBRATED ||
			   calibrationLightPress_[i][1] == UNCALIBRATED ||
			   calibrationHeavyPress_[i][1] == UNCALIBRATED ||
			   calibrationQuiescent_[i][2] == UNCALIBRATED ||
			   calibrationLightPress_[i][2] == UNCALIBRATED ||
			   calibrationHeavyPress_[i][2] == UNCALIBRATED)
			{
				cout << "ERROR: Key " << i + 21 << " did not properly calibrate.\n";
				changeKeyState(i, kKeyStateDisabled);
				calibrationHadErrors = true;
			}	
			
			// Prevent divide-by-0 errors later!
			if(calibrationLightPress_[i][0] == calibrationQuiescent_[i][0])
				calibrationLightPress_[i][0]++;			
			if(calibrationLightPress_[i][1] == calibrationQuiescent_[i][1])
				calibrationLightPress_[i][1]++;	
			if(calibrationLightPress_[i][2] == calibrationQuiescent_[i][2])
				calibrationLightPress_[i][2]++;	
			if(calibrationLightPress_[i][3] == calibrationQuiescent_[i][3])
				calibrationLightPress_[i][3]++;	
			
			// Reading key pressure requires a minimum differential between light and heavy key presses
			// On black keys, the heavy presses have the larger value
			if(calibrationHeavyPress_[i][0] - calibrationLightPress_[i][0] > MIN_KEY_PRESSURE_DIFF &&
			   calibrationHeavyPress_[i][1] - calibrationLightPress_[i][1] > MIN_KEY_PRESSURE_DIFF &&
			   calibrationHeavyPress_[i][2] - calibrationLightPress_[i][2] > MIN_KEY_PRESSURE_DIFF)
				calibrationCanReadKeyPressure_[i] = true;
			else
			{
				calibrationCanReadKeyPressure_[i] = false;
				cout << "Warning: Can't read key pressure on [black] key " << i + 21 << endl;		
			}			
		}
	}
	
	// Reset this every time in case a key was stuck before
	for(i = 0; i < 88; i++)
	{
		keyIdleThreshold_[i] = 200;
	}		

	return calibrationHadErrors;
}


// Load calibration values from a file (created with saveCalibrationToFile).  Returns true on success.

bool PianoBarController::loadCalibrationFromFile(string& filename)
{
	int i, j;

	pthread_mutex_lock(&audioMutex_);
	// To begin with, clear all existing values (then let the file data fill them in).	
	calibrationStatus_ = kPianoBarNotCalibrated;

	for(i = 0; i < 88; i++)
	{
		for(j = 0; j < 4; j++)
		{
			calibrationQuiescent_[i][j] = calibrationLightPress_[i][j] = calibrationHeavyPress_[i][j] = UNCALIBRATED;
		}
	}
	
	// Open the file and read the new values
	try
	{
		// Format: "[note#] [sequence] [quiescent] [light] [heavy]"
		// Parse the calibration text table
		ifstream inputFile;
		int key, seqOffset, quiescent, light, heavy;
		
		inputFile.open(filename.c_str(), ios::in);
		if(inputFile.fail())		// Failed to open file...
		{
			pthread_mutex_unlock(&audioMutex_);
			return false;
		}
		while(!inputFile.eof())
		{
			inputFile >> key;
			inputFile >> seqOffset;
			inputFile >> quiescent;
			inputFile >> light;
			inputFile >> heavy;
			
			if(key < 0 || key > 87)
			{
				cerr << "loadCalibrationFromFile(): Invalid key " << key << endl;
				inputFile.close();
				pthread_mutex_unlock(&audioMutex_);
				return false;
			}
			if(seqOffset < 0 || seqOffset > 3)
			{
				cerr << "loadCalibrationFromFile(): Invalid offset " << seqOffset << " for key " << key << endl;
				inputFile.close();
				pthread_mutex_unlock(&audioMutex_);
				return false;				
			}
			
			calibrationQuiescent_[key][seqOffset] = quiescent;
			calibrationLightPress_[key][seqOffset] = light;
			calibrationHeavyPress_[key][seqOffset] = heavy;
		}
		inputFile.close();		
	}
	catch(...)
	{
		pthread_mutex_unlock(&audioMutex_);
		return false;
	}
	
	cleanUpCalibrationValues();	// Ignore whether or not this has errors for purposes of return value...
	
	calibrationStatus_ = kPianoBarCalibrated;
	pthread_mutex_unlock(&audioMutex_);
	
	return true;
}

void PianoBarController::printKeyStatus()
{
	if(!isInitialized_)
	{
		cout << "Piano Bar not initialized.\n";
		return;
	}
	if(!isRunning_)
	{
		cout << "Piano Bar not running.\n";
		return;
	}
	switch(calibrationStatus_)
	{
		case kPianoBarNotCalibrated:
			cout << "Piano Bar not calibrated.\n";
			return;
		case kPianoBarInCalibration:
			cout << "Piano Bar in calibration.\n";
			return;
		case kPianoBarCalibrated:
			cout << "Octave 0:                                                       A     A#    B\n";
			printKeyStatusHelper(0, 3, strlen("Octave 0:                                                       "));
			cout << "\nOctave 1: C     C#    D     D#    E     F     F#    G     G#    A     A#    B\n";
			printKeyStatusHelper(3, 12, strlen("Octave 1: "));
			cout << "\nOctave 2: C     C#    D     D#    E     F     F#    G     G#    A     A#    B\n";
			printKeyStatusHelper(15, 12, strlen("Octave 1: "));
			cout << "\nOctave 3: C     C#    D     D#    E     F     F#    G     G#    A     A#    B\n";
			printKeyStatusHelper(27, 12, strlen("Octave 1: "));
			cout << "\nOctave 4: C     C#    D     D#    E     F     F#    G     G#    A     A#    B\n";
			printKeyStatusHelper(39, 12, strlen("Octave 1: "));
			cout << "\nOctave 5: C     C#    D     D#    E     F     F#    G     G#    A     A#    B\n";
			printKeyStatusHelper(51, 12, strlen("Octave 1: "));
			cout << "\nOctave 6: C     C#    D     D#    E     F     F#    G     G#    A     A#    B\n";
			printKeyStatusHelper(63, 12, strlen("Octave 1: "));
			cout << "\nOctave 7: C     C#    D     D#    E     F     F#    G     G#    A     A#    B     C\n";
			printKeyStatusHelper(75, 13, strlen("Octave 1: "));
			return;
		default:
			cout << "Piano Bar: Unknown calibration status.\n";
			return;
	}
}

// Destructor needs to close the currently open device

PianoBarController::~PianoBarController()
{
	close();
	pthread_mutex_destroy(&audioMutex_);
	freeKeyQuiescentModel();
}

#pragma mark --- Private Methods ---

// Private helper function prints state and position of <length> keys starting at <start>

void PianoBarController::printKeyStatusHelper(int start, int length, int padSpaces)
{
	int i;
	const char *shortStateNames[kKeyStatesLength] = {"Unk.  ", "Idle  ", "PreT  ", "PreV  ", "Tap   ",
		"Press ", "Down  ", "AftT  ", "AftV  ", "Rel.  ", "*D/A* "};
	
	// On the first line, print the state of each key
	
	for(i = 0; i < padSpaces; i++)
		cout << " ";
	for(i = start; i < start + length; i++)
		cout << shortStateNames[currentState(i)];
	cout << endl;
	
	// On the second line, print the current position

	for(i = 0; i < padSpaces; i++)
		cout << " ";
	for(i = start; i < start + length; i++)
		printf("%-6d", runningPositionAverage(i, 0, (kPianoBarKeyColor[i] == K_W) ? 10 : 30));
	cout << endl;
}


#pragma mark Audio Input

// Callback from portaudio when new data is available.  In this function, pull it apart to separate out individual keys.

int PianoBarController::audioCallback(const void *input, void *output, 
									  unsigned long frameCount, const PaStreamCallbackTimeInfo* timeInfo,
									  PaStreamCallbackFlags statusFlags)
{
	int readCount, sequence;
	short key, value, type;
	short *inData = (short *)input;
	
	pthread_mutex_lock(&audioMutex_);
	
	for(readCount = 0; readCount < frameCount; readCount++)
	{
		currentTimeStamp_++;
		
		// data format: 10 channels at 16 bits each
		// ch0: flags
		//		15-12: reserved
		//		11-8: sequence number, for skip detection
		//		7-6: reserved
		//		5-1: sequence counter-- holds values 0-17
		//		0: data valid test, should always be 0
		// ch1-9: (144 bits)
		//		packed 12-bit little-endian values of 12 channels
		
		if(inData[0] & 0x0001)
		{
			cerr << "PianoBarController warning: parity error in PianoBar data (data = " << inData[0];
			cerr << " count = " << readCount << ")\n";
			
			// Just don't increment the history.  This will lead to some weird time stretching, but
			// it's the easiest option for now.
			continue;					
		}
		
		sequence = (int)(inData[0] & 0x003E) >> 1;
		
		if(sequence > 17 || sequence < 0)
		{
			cerr << "PianoBarController warning: sequence " << sequence << " out of range (data = ";
			cerr << inData[0] << " count = " << readCount << ")\n";
			continue;
		}
		
		// Retrieve key numbers (0 for unused slots), types (white/black, cycle number), and value
		//   (signed 12-bit, -2048 to 2047) for each group
		// Piano Bar groups are interleaved between ADCs.  Order: 1 2 5 6 9 10 3 4 7 8 11 12
		//   This is because the second two groups of each board (3,4,7,8,11,12) are offset in phase by half
		//   a cycle with respect to the first two groups.
		
		key = kPianoBarMapping[sequence][0];
		type = kPianoBarSignalTypes[sequence][0];
		value = (inData[1] & 0x00FF) + ((inData[1] & 0xF000) >> 4);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][1];
		type = kPianoBarSignalTypes[sequence][1];
		value = ((inData[1] & 0x0F00) >> 4) + ((inData[2] & 0x00F0) >> 4) + ((inData[2] & 0x000F) << 8);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][4];
		type = kPianoBarSignalTypes[sequence][4];
		value = ((inData[2] & 0xFF00) >> 8) + ((inData[3] & 0x00F0) << 4);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][5];
		type = kPianoBarSignalTypes[sequence][5];
		value = ((inData[3] & 0x000F) << 4) + ((inData[3] & 0xF000) >> 12) + (inData[3] & 0x0F00);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][8];
		type = kPianoBarSignalTypes[sequence][8];
		value = (inData[4] & 0x00FF) + ((inData[4] & 0xF000) >> 4);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][9];
		type = kPianoBarSignalTypes[sequence][9];
		value = ((inData[4] & 0x0F00) >> 4) + ((inData[5] & 0x00F0) >> 4) + ((inData[5] & 0x000F) << 8);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][2];
		type = kPianoBarSignalTypes[sequence][2];
		value = ((inData[5] & 0xFF00) >> 8) + ((inData[6] & 0x00F0) << 4);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][3];
		type = kPianoBarSignalTypes[sequence][3];
		value = ((inData[6] & 0x000F) << 4) + ((inData[6] & 0xF000) >> 12) + (inData[6] & 0x0F00);
		value = (value > 2047 ? value - 4096 : value);				
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][6];
		type = kPianoBarSignalTypes[sequence][6];
		value = (inData[7] & 0x00FF) + ((inData[7] & 0xF000) >> 4);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][7];
		type = kPianoBarSignalTypes[sequence][7];
		value = ((inData[7] & 0x0F00) >> 4) + ((inData[8] & 0x00F0) >> 4) + ((inData[8] & 0x000F) << 8);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][10];
		type = kPianoBarSignalTypes[sequence][10];
		value = ((inData[8] & 0xFF00) >> 8) + ((inData[9] & 0x00F0) << 4);
		value = (value > 2047 ? value - 4096 : value);
		processValue(key, type, value);
		
		key = kPianoBarMapping[sequence][11];
		type = kPianoBarSignalTypes[sequence][11];
		value = ((inData[9] & 0x000F) << 4) + ((inData[9] & 0xF000) >> 12) + (inData[9] & 0x0F00);
		value = (value > 2047 ? value - 4096 : value);							
		processValue(key, type, value);
		
		inData += 10;	// Each frame is 10 samples of 16 bits
		
		if(calibrationStatus_ == kPianoBarInCalibration)
			calibrationSamples_++;
	}
	
	// There are two possible approaches to the timing of actuator control.  One is to send only one
	// action per audio buffer which reflects the entire data stored within it.  The other is to send multiple
	// actions, deliberately delaying ones that happen later in the buffer.  The first approach snaps everything
	// to the granularity of the buffer size (e.g. 3ms for buffer size 32), where the second one preserves time
	// linearity but adds delay.
	
	updateKeyQuiescentModel();
	updateKeyStates();
	sendKeyStateMessages();	// This could eventually be called more frequently than updateKeyStates()
	
	pthread_mutex_unlock(&audioMutex_);
	
	return paContinue;
}

// Store a new value from the audio callback into the history buffer.  If we're in calibration, also examine
// the raw values for calibration purposes

void PianoBarController::processValue(short midiNote, short type, short value)
{
	bool white;
	int key, calibratedValueInt, seqOffset;
	
	// Though the Piano Bar reports multiple data points per sequence for some white keys, a consistent
	// approach across the keyboard is preferred.  Therefore, we'll only pay attention to the first white
	// key sample per sequence, and the first three black key samples (Bb7 samples 4 times).
	
	switch(type)
	{
		case PB_W1:
			white = true;
			seqOffset = 0;
			break;
		case PB_W2:
			if(calibrationStatus_ != kPianoBarInCalibration)
				return;
			white = true;
			seqOffset = 1;
			break;
		case PB_W3:
			if(calibrationStatus_ != kPianoBarInCalibration)
				return;			
			white = true;
			seqOffset = 2;
			break;
		case PB_W4:
			if(calibrationStatus_ != kPianoBarInCalibration)
				return;			
			white = true;
			seqOffset = 3;
			break;			
		case PB_B1:			
			white = false;
			seqOffset = 0;
			break;
		case PB_B2:
			white = false;
			seqOffset = 1;
			break;
		case PB_B3:
			white = false;
			seqOffset = 2;
			break;
		case PB_B4:
			if(calibrationStatus_ != kPianoBarInCalibration)
				return;			
			white = false;
			seqOffset = 3;
		case PB_NA:
		default:
			return;
	}
	
	if(midiNote < 21 || midiNote > 108)		// Shouldn't happen, but...
		return;								// can't do anything with something outside the piano range
	key = midiNote - 21;					// Convert to buffer index
		
	
	switch(calibrationStatus_)
	{
		case kPianoBarNotCalibrated:
			// Store the raw value in the history buffer
			if(keyHistory_[key] == NULL)
				return;
			
			keyHistoryPosition_[key] = (keyHistoryPosition_[key] + 1) % keyHistoryLength_[key];			
			keyHistory_[key][keyHistoryPosition_[key]] = value;
			keyHistoryTimestamps_[key][keyHistoryPosition_[key]] = currentTimeStamp_;
			break;
		case kPianoBarCalibrated:
			// Scale the raw value by the calibration settings to produce a normalized value where 0 = not pressed,
			// 4096 = pressed, 4352 = heavy pressure.
			
			if(keyHistory_[key] == NULL)
				return;

			if(white)
			{
				if(value < calibrationLightPress_[key][seqOffset] && calibrationCanReadKeyPressure_[key])
				{
					// Low value means heavy press
					calibratedValueInt = 4096 + (256*(int)(value - calibrationLightPress_[key][seqOffset]) / 
												 (int)(calibrationHeavyPress_[key][seqOffset] - calibrationLightPress_[key][seqOffset]));
				}
				else
					calibratedValueInt = (4096*(int)(value - calibrationQuiescent_[key][seqOffset]) / 
										  (int)(calibrationLightPress_[key][seqOffset] - calibrationQuiescent_[key][seqOffset]));
			}
			else
			{
				if(value > calibrationLightPress_[key][seqOffset] && calibrationCanReadKeyPressure_[key])
				{
					// High value means heavy press
					calibratedValueInt = 4096 + (256*(int)(value - calibrationLightPress_[key][seqOffset]) / 
												 (int)(calibrationHeavyPress_[key][seqOffset] - calibrationLightPress_[key][seqOffset]));
				}
				else
					calibratedValueInt = (4096*(int)(value - calibrationQuiescent_[key][seqOffset]) / 
										  (int)(calibrationLightPress_[key][seqOffset] - calibrationQuiescent_[key][seqOffset]));				
			}

			keyHistoryPosition_[key] = (keyHistoryPosition_[key] + 1) % keyHistoryLength_[key];
			keyHistory_[key][keyHistoryPosition_[key]] = calibratedValueInt;
			keyHistoryTimestamps_[key][keyHistoryPosition_[key]] = currentTimeStamp_;
			break;
		case kPianoBarInCalibration:
			calibrationHistoryPosition_[key][seqOffset] = (calibrationHistoryPosition_[key][seqOffset] + 1) % calibrationHistoryLength_;
			calibrationHistory_[key][seqOffset][calibrationHistoryPosition_[key][seqOffset]] = value;
			
			// Quiescent levels have been set when calibration began.  Watch for significant changes to set key press levels
			// Check for end of key press by comparing last M samples against the N samples before that.  If they
			// (approximately) match, the key press is done.
			
			if(calibrationQuiescent_[key][seqOffset] == UNCALIBRATED)	// Don't go any further until we've set quiescent value
				break;
			
			if(keysToCalibrate_ != NULL)
			{
				bool foundMatch = false;
				int k;
				
				for(k = 0; k < keysToCalibrate_->size(); k++)
				{
					if((*keysToCalibrate_)[k] == key + 21)
					{
						foundMatch = true;
						break;
					}
				}
				
				if(!foundMatch)
					break;
			}
			
			if(white)
			{
				// Values below quiescent indicate a key press event
				if(value < (int)(((calibrationQuiescent_[key][seqOffset]*8)/10)))
				{
					int currentAverage = calibrationRunningAverage(key, seqOffset, 0, 8);
								
					//cout << "key " << key + 21 << " down (value = " << value << ")\n";						
						
					if(calibrationOkToCalibrateLight_[key][seqOffset])
					{
						int pastAverage = calibrationRunningAverage(key, seqOffset, 8, 16);
						if(abs(currentAverage - pastAverage) <= 2)
						{
							cout << "key " << key + 21 << "/" << seqOffset << ": light = " << currentAverage << endl;
							calibrationLightPress_[key][seqOffset] = (short)currentAverage;
							calibrationOkToCalibrateLight_[key][seqOffset] = false;		
						}
					}
					
					// Heavy press is the minimum overall value
					if(currentAverage < (int)calibrationHeavyPress_[key][seqOffset] ||
					   calibrationHeavyPress_[key][seqOffset] == UNCALIBRATED)
						calibrationHeavyPress_[key][seqOffset] = (short)currentAverage;					
				}
				else if(value >= (int)calibrationQuiescent_[key][seqOffset])
					calibrationOkToCalibrateLight_[key][seqOffset] = true;				
			}
			else // black keys
			{
				// Values above quiescent indicate a key press event
				if(value > (int)(calibrationQuiescent_[key][seqOffset] + 100))
				{
					int currentAverage = calibrationRunningAverage(key, seqOffset, 0, 8);
				
					//cout << "key " << key + 21 << " down (value = " << value << ")\n";	
					if(calibrationOkToCalibrateLight_[key][seqOffset])
					{				
						int pastAverage = calibrationRunningAverage(key, seqOffset, 8, 16);
						if(abs(currentAverage - pastAverage) <= 2)
						{
							cout << "key " << key + 21 << "/" << seqOffset << ": light = " << currentAverage << endl;
							calibrationLightPress_[key][seqOffset] = (short)currentAverage;
							calibrationOkToCalibrateLight_[key][seqOffset] = false;
						}
					}
					
					// Heavy press is the maximum overall value
					if(currentAverage > (int)calibrationHeavyPress_[key][seqOffset] ||
					   calibrationHeavyPress_[key][seqOffset] == UNCALIBRATED)
						calibrationHeavyPress_[key][seqOffset] = (short)currentAverage;
				}
				else if(value <= (int)calibrationQuiescent_[key][seqOffset])
					calibrationOkToCalibrateLight_[key][seqOffset] = true;
			}
			break;
	}
}

#pragma mark Motion Analysis

// Return the average of the last N points for a particular key
// key = 0 to 87 (not MIDI note number)

int PianoBarController::runningPositionAverage(int key, int offset, int length)
{
	int sum = 0, loc;
	int i;
	
	if(length == 0)
		return 0;
	
	loc = (keyHistoryPosition_[key] - offset - length + keyHistoryLength_[key]) % keyHistoryLength_[key];
	
	for(i = 0; i < length; i++)
	{
		sum += keyHistory_[key][loc];
		loc = (loc + 1) % keyHistoryLength_[key];
	}
	
	return sum / length;
}

// Return the average velocity over the last N points for a particular key
// key = 0 to 87
// Since we're doing this all with integer math and velocity numbers can be pretty
// small, multiply the result by a scaler so we get better resolution

int PianoBarController::runningVelocityAverage(int key, int offset, int length)
{	
	int scaler;
	
	// Velocity is the first difference (more-or-less).  And the handy thing about summing
	// a string of first differences is that they telescope.
	
	if(length == 0)
		return 0;
	
	int start = (keyHistoryPosition_[key] - offset - length + keyHistoryLength_[key]) % keyHistoryLength_[key];
	int finish = (keyHistoryPosition_[key] - offset + keyHistoryLength_[key]) % keyHistoryLength_[key];

	// Black keys sample three times as frequently, so we need to compensate for that in the scaling of the result
	if(kPianoBarKeyColor[key] == K_B)
		scaler = VELOCITY_SCALER * 3;
	else
		scaler = VELOCITY_SCALER;
	
	if(length > 1)
		return scaler*(keyHistory_[key][finish] - keyHistory_[key][start]) / length;
	return scaler*(keyHistory_[key][finish] - keyHistory_[key][start]);
}

// Return the average acceleration over the last N points for a particular key
// key = 0 to 87
// Since we're doing this all with integer math and velocity numbers can be pretty
// small, multiply the result by a scaler so we get better resolution

int PianoBarController::runningAccelerationAverage(int key, int offset, int length)
{	
	int scaler;
	
	// Velocity is the first difference (more-or-less).  And the handy thing about summing
	// a string of first differences is that they telescope.
	
	if(length == 0)
		return 0;
	
	int start = runningVelocityAverage(key, offset + length, 1);
	int finish = runningVelocityAverage(key, offset, 1);
	
	// Black keys sample three times as frequently, so we need to compensate for that in the scaling of the result
	if(kPianoBarKeyColor[key] == K_B)
		scaler = 3;
	else
		scaler = 1;
	
	if(length > 1)
		return scaler*(finish - start) / length;
	return scaler*(finish - start);
}

// Find the maximum acceleration value within the specified window

int PianoBarController::peakAcceleration(int key, int offset, int distanceToSearch, int samplesToAverage, bool positive)
{
	int i, acc;
	//int pastPeakCounter = 0;
	int peakValue = positive ? -0xFFFFFF : 0xFFFFFF;
	
	for(i = 0; i < distanceToSearch; i += samplesToAverage)
	{
		acc = runningAccelerationAverage(key, i + offset, samplesToAverage);
		
		if(positive)
		{
			if(acc > peakValue)
				peakValue = acc;
		}
		else
		{
			if(acc < peakValue)
				peakValue = acc;
		}
	}
	
	return peakValue;
}

// Return the average of the last N points for a particular key, with a particular place within
// the cycle.
// key = 0 to 87 (not MIDI note number)
// offset = number of samples back to go

int PianoBarController::calibrationRunningAverage(int key, int seq, int offset, int length)
{
	int sum = 0, loc, count = 0;;
	int i;
	
	loc = (calibrationHistoryPosition_[key][seq] - offset - length + calibrationHistoryLength_) % calibrationHistoryLength_;
	
	for(i = 0; i < length; i++)
	{
		if(calibrationHistory_[key][seq][loc] != UNCALIBRATED)
		{
			sum += calibrationHistory_[key][seq][loc];
			count++;
		}
		loc = (loc + 1) % calibrationHistoryLength_;
	}
	
	if(count == 0)
		return UNCALIBRATED;
	else if(count < length)
		cout << "Warning: running average for key " << key + 21 << " seq " << seq << " encountered " << length - count << " uncalibrated values.\n";

	return sum / count;
}

#pragma mark State Machine

// In this function we examine the recent key position data to determine the state of each key.
// We'll potentially look at position, velocity (1st diff), and acceleration (2nd diff) to figure this out.

// Allowable state transitions:
//	 Unknown -->	Idle
//   Idle -->		Pretouch, Tap, Press
//	 Pretouch -->	Idle, PreVibrato, Tap, Press
//	 PreVibrato --> Idle, Pretouch, Press
//	 Tap -->		Idle, Pretouch, Press
//	 Press -->		Down
//	 Down -->		Aftertouch, Release
//	 Aftertouch --> Down, AfterVibrato, Release
//	 AfterVibrato --> Down, Aftertouch, Release
//	 Release -->	Idle, Pretouch

void PianoBarController::updateKeyStates()
{
	int key, pos, vel, acc, sampleLength, i, j;
	bool white, canGoIdle, shouldAbort;
	
	for(key = 0; key < 88; key++)
	{
		// For each key, the state transition depends on its existing state (see chart above)
		// Not all states can transition to all other states
		
		white = (kPianoBarKeyColor[key] == K_W);
		
		// NOTE: Never use return; in this block, only break;
		
		switch(currentState(key))
		{
			case kKeyStateIdle:
				// Don't want to always be calculating a zillion averages.  Look only at the most recent
				// data point to see if anything has happened.

				if(lastPosition(key) < keyIdleThreshold_[key])
					break;
				
				sampleLength = white ? 6 : 18;
				
				// Now check the running average to see if it meets our threshold				
				pos = runningPositionAverage(key, 0, sampleLength);
				acc = peakAcceleration(key, 0, white ? 30 : 90, 2, true);				
				//cout << "acceleration " << acc << endl;				

				if(pos < keyIdleThreshold_[key]*2 + predictedQuiescentValue(key) /*&& acc < keyTapAccelerationThreshold_*/)
					break;
				
				if(pos > keyPressPositionThreshold_)
					changeKeyState(key, kKeyStatePress);
				else if(previousState(key) != kKeyStateRelease)
				{
					keyVibratoCount_[key] = 0;
					if(acc >= keyTapAccelerationThreshold_)
						changeKeyState(key, kKeyStateTap);
					else if(pos >= keyIdleThreshold_[key]*2 + predictedQuiescentValue(key))
						changeKeyState(key, kKeyStatePretouch);
				}
				else
				{
					// Need to debounce the key mechanism by ignoring the first bit of data after a key
					// release.  Otherwise it will appear as another pretouch gesture.
					
					if(framesInCurrentState(key) > keyIdleBounceTime_)
						changeKeyState(key, kKeyStateIdle);	// Set "idle" again to say we've finished post-release period
				}
				break;
			case kKeyStatePretouch:
				// The end of a pretouch gesture should be typified by a comparatively long period of key rest
				// meaning both idle position and low velocity.

				//if(kPianoBarKeyColor[key] == K_W && debugPrintGate(key, 1000))
				//	cout << "key " << key << " predicted " << predictedQuiescentValue(key) << " actual " << lastPosition(key) << endl;
				
				sampleLength = white ? 6 : 18;
				canGoIdle = true;
				
				for(i = 3; i >= 0; i--)	// see below before changing this
				{
					pos = runningPositionAverage(key, i*sampleLength, sampleLength);
					vel = runningVelocityAverage(key, i*sampleLength, sampleLength);
					
					if(pos > keyIdleThreshold_[key] + predictedQuiescentValue(key) || vel > keyPretouchIdleVelocity_)
						canGoIdle = false;
				}
				
				if(canGoIdle)
				{
					changeKeyState(key, kKeyStateIdle);
					break;
				}
				
				// See if a vibrato action has been initiated.  This will be marked by noticeable alternating
				// positive and negative peaks in velocity.  Parity of the vibrato count variable tells us which
				// peak to look for.
				
				if(keyVibratoCount_[key] % 2)
				{
					if(vel > keyPreVibratoVelocity_ && currentTimeStamp_ > (keyLastVibratoTimestamp_[key] + keyPreVibratoFrameSpacing_))
					{
						//cout << "vib " << keyVibratoCount_[key] << ": vel " << vel << " frame " << framesInCurrentState(key) << endl;
						keyVibratoCount_[key]++;
						keyLastVibratoTimestamp_[key] = currentTimeStamp_;
					}
				}
				else
				{
					if(vel < -keyPreVibratoVelocity_ && currentTimeStamp_ > (keyLastVibratoTimestamp_[key] + keyPreVibratoFrameSpacing_))
					{
						//cout << "vib " << keyVibratoCount_[key] << ": vel " << vel << " frame " << framesInCurrentState(key) << endl;
						keyVibratoCount_[key]++;
						keyLastVibratoTimestamp_[key] = currentTimeStamp_;
					}					
				}
				
				if(keyVibratoCount_[key] > 4)
				{
					changeKeyState(key, kKeyStatePreVibrato);	// counter that we'll use later to detect the end of the gesture
					break;
				}
				
				// Else look for press action-- pos and vel are left as the most recent measurements from
				// the loop above
				
				if(/*vel > keyPressVelocityThreshold_ || */pos > keyPressPositionThreshold_)
				{
					changeKeyState(key, kKeyStatePress);
					break;
				}

				// FIXME: update for new timing model!
				/*if(abs(pos - keyLastPosition_[key]) < keyStuckPositionThreshold_)
				{
					keyStateStuckCounter_[key]++;
					if(keyStateStuckCounter_[key] > 50000 / bufferSize_)
					{
						cout << "KEY " << key+21 << " STUCK: Set new idle threshold to " << (float)pos*1.05 << endl;
						
						keyIdleThreshold_[key] = pos*21/20;
					}
				}
				keyLastPosition_[key] = pos;*/
				break;
			case kKeyStatePreVibrato:
				// First, see if we can go straight to idle (same code as Pretouch):
				
				sampleLength = white ? 6 : 18;
				canGoIdle = true;
				
				for(i = 3; i >= 0; i--)	// see below before changing this
				{
					pos = runningPositionAverage(key, i*sampleLength, sampleLength);
					vel = runningVelocityAverage(key, i*sampleLength, sampleLength);
					
					if(pos > keyIdleThreshold_[key] + predictedQuiescentValue(key) || vel > keyPretouchIdleVelocity_)
						canGoIdle = false;
				}
				
				if(canGoIdle)
				{
					changeKeyState(key, kKeyStateIdle);
					break;
				}
		
				// Also keep an eye out for key press
				
				if(pos > keyPressPositionThreshold_)
				{
					changeKeyState(key, kKeyStatePress);
					break;
				}
				
				// Next, watch for the continuation of the vibrato gesture, and specifically for a timeout
				// that suggests it has finished.  
				
				if(keyVibratoCount_[key] % 2)
				{
					if(vel > keyPreVibratoVelocity_ && currentTimeStamp_ > (keyLastVibratoTimestamp_[key] + keyPreVibratoFrameSpacing_))
					{
						//cout << "vib " << keyVibratoCount_[key] << ": vel " << vel << " frame " << framesInCurrentState(key) << endl;
						keyVibratoCount_[key]++;
						keyLastVibratoTimestamp_[key] = currentTimeStamp_;
					}
				}
				else
				{
					if(vel < -keyPreVibratoVelocity_ && currentTimeStamp_ > (keyLastVibratoTimestamp_[key] + keyPreVibratoFrameSpacing_))
					{
						//cout << "vib " << keyVibratoCount_[key] << ": vel " << vel << " frame " << framesInCurrentState(key) << endl;
						keyVibratoCount_[key]++;
						keyLastVibratoTimestamp_[key] = currentTimeStamp_;
					}					
				}
				
				// Go back to Pretouch on timeout, since we've already checked on going to Idle and Press
				
				if(currentTimeStamp_ - keyLastVibratoTimestamp_[key] > keyPreVibratoTimeout_)
				{
					keyVibratoCount_[key] = keyLastVibratoTimestamp_[key] = 0;
					changeKeyState(key, kKeyStatePretouch);
				}
				break;
			case kKeyStateTap:
				// For now, go straight to pretouch
				//if(framesInCurrentState(key) >= secondsToFrames(0.05))
					changeKeyState(key, kKeyStatePretouch);
				break;
			case kKeyStatePress:
				// Look for the point at which the key position is no longer increasing.  This is the spot
				// at which it goes from being in press to being down.  Start by looking back until we get a value
				// that's below the press position threshold, then work forward until we find the exact point.
				// If we don't hit it yet, we're still "in press"
				
				i = keyHistoryPosition_[key];
				j = 0;
				
				shouldAbort = false;
				
				while(keyHistory_[key][i] > keyPressPositionThreshold_)	// Look for partially-depressed state
				{
					i = (i - 1 + keyHistoryLength_[key]) % keyHistoryLength_[key];
					j++;
					if(j >= keyHistoryLength_[key])
					{
						shouldAbort = true;
						break;
					}
				}
				
				if(shouldAbort)
					break;
				
				sampleLength = white ? 3 : 9;				
				vel = runningVelocityAverage(key, j, sampleLength);
				
				while(runningVelocityAverage(key, j, sampleLength) > 0)	// Search forwards for end of key press
				{
					j--;
					if(j < 0)
					{
						shouldAbort = true;
						break;
					}
				}
				
				if(shouldAbort)
					break;
				
				keyDownVelocity_[key] = vel;
				cout << "Key press velocity " << vel << endl;
				
				changeKeyStateWithTimestamp(key, kKeyStateDown, timestampForOffset(key, j));		
				keyDownInitialPosition_[key] = 0;		// This gets set properly later in Down state
				break;
			case kKeyStateDown:
				// Use a position metric to watch for key release: below this value and we switch to release state
				sampleLength = white ? 6 : 18;			
				pos = runningPositionAverage(key, 0, sampleLength);				
				
				if(pos < keyReleasePositionThreshold_)
					changeKeyState(key, kKeyStateRelease);
				
				// Otherwise, look for aftertouch.  We have to be in the down state some minimum amount of time for
				// this to make sense.
				if(framesInCurrentState(key) < keyDownBounceTime_)
					break;
	
				// At this point, gather the initial position of the key to compare later for aftertouch
				if(keyDownInitialPosition_[key] == 0)
				{
					sampleLength = white ? 12 : 24;		// ca. 20ms (take a longer sample than usual to get a clean reference value)
					
					keyDownInitialPosition_[key] = runningPositionAverage(key, 
																		  timestampToKeyOffset(key, framesInCurrentState(key) - keyDownBounceTime_),
																		  sampleLength);
					cout << "Key " << key << ": initial position " << keyDownInitialPosition_[key] << endl;
					break;
				}
				
				// Now we can compare the current position to the initial one to detect aftertouch
				sampleLength = white ? 6 : 18;			
				pos = runningPositionAverage(key, 0, sampleLength);
				
				if(abs(pos - keyDownInitialPosition_[key]) > keyAftertouchThreshold_ && kPianoBarKeyColor[key] == K_W)	// No direct black key aftertouch for now.
					changeKeyState(key, kKeyStateAftertouch);
				break;
			case kKeyStateAftertouch:
				// Here we look for key release just like the down state
				// Use a position metric to watch for key release: below this value and we switch to release state
				sampleLength = white ? 6 : 18;			
				pos = runningPositionAverage(key, 0, sampleLength);				
				
				if(pos < keyReleasePositionThreshold_)
				{
					changeKeyState(key, kKeyStateRelease);
					break;
				}
				
				//if(debugPrintGate(key, 1000))
				//	cout << "Key " << key << ": position " << pos << endl;
				break;
			case kKeyStateAfterVibrato:
				// Later
				break;
			case kKeyStateRelease:
				// Release can go back to idle or (eventually) to pretouch -- or should it be after-release?
				sampleLength = white ? 6 : 18;			
				pos = runningPositionAverage(key, 0, sampleLength);		
				
				if(pos < keyIdleThreshold_[key] + predictedQuiescentValue(key))
					changeKeyState(key, kKeyStateIdle);	
				else
				{
					vel = runningVelocityAverage(key, 0, sampleLength);	
					if(pos > keyPressPositionThreshold_ || vel > keyPressVelocityThreshold_)
						changeKeyState(key, kKeyStatePress);
				}
				break;
			case kKeyStateUnknown:			// Allow unknown state to return to idle
				if(lastPosition(key) < keyIdleThreshold_[key] + predictedQuiescentValue(key))
					changeKeyState(key, kKeyStateIdle);
				break;
			case kKeyStateDisabled:			// Disabled is a permanent state...
			default:
				break;
		}
	}
	
	lastStateUpdate_ = currentTimeStamp_;	// Indicate that we just performed a state update
}

// This method queries each key for its state and sends messages out to the synth whose exact
// format depend on the state.  In other words, this method controls the mapping from key behavior
// to actuator parameters.  For now, it's hard-coded but in the future we'll want to make this completely
// dynamic.

void PianoBarController::sendKeyStateMessages()
{
	int key, pos, vel, acc;
	double intensity, pitch, brightness;
	bool white;
	set<int> keysToSkip;			// Indicate keys that should be skipped (as they are part of a multi-key gesture)
	RealTimeMidiNote *note;
	
	// First, scan for multi-key gestures
	
	handleMultiKeyPitchBend(&keysToSkip);
	handleMultiKeyHarmonicSweep(&keysToSkip);
	
	for(key = 0; key < 88; key++)
	{
		if(keysToSkip.count(key) > 0)	// If this happens, it's because the key indicated is part of a multi-key
			continue;					// gesture that overrides the individual key behavior
			
		// For each key, the state transition depends on its existing state (see chart above)
		// Not all states can transition to all other states
		
		white = (kPianoBarKeyColor[key] == K_W);
		
		switch(currentState(key))
		{
			case kKeyStateTap:
			case kKeyStatePretouch:
				note = noteForKey(key);
				if(note == NULL)
					break;

				vel = runningVelocityAverage(key, 0, white ? 6 : 18);
				if(vel < keyPretouchHoldoffVelocity_)
				{
					acc = runningAccelerationAverage(key, 0, timestampToKeyOffset(key, lastStateMessage_));
					intensity = ((double)lastPosition(key) - (double)keyIdleThreshold_[key]) / 
								((double)keyPressPositionThreshold_ - (double)keyIdleThreshold_[key]);
					intensity += ((double)acc) / 3000.0;
					
					if(currentState(key) == kKeyStateTap)	// Give taps an amplitude boost
						intensity *= 2.0;
					
					if(intensity > 1.0)
						intensity = 1.0;
					if(intensity < 0.0)
						intensity = 0.0;
				}
				else if(currentState(key) != kKeyStateTap && previousState(key) != kKeyStateTap)
				{
					//if(debugPrintGate(key, 100))
					//	cout << "holdoff!\n";
					intensity = 0.0;
				}
				
				//cout << "intensity " << intensity << endl;
				note->setAbsoluteIntensityBase(intensity);
				note->updateSynthParameters();
			
				break;
			case kKeyStatePreVibrato:
				note = noteForKey(key);
				if(note == NULL)
					break;
				// Take the absolute value of velocity and use it to set the relative pitch, creating a gliss
				vel = runningVelocityAverage(key, 0, timestampToKeyOffset(key, lastStateMessage_));
				pitch = fabs((double)vel / 1000.0);

				intensity = ((double)lastPosition(key) - (double)keyIdleThreshold_[key]) / 
				((double)keyPressPositionThreshold_ - (double)keyIdleThreshold_[key]);
				if(intensity > 1.0)
					intensity = 1.0;
				if(intensity < 0.0)
					intensity = 0.0;				
				
				note->setAbsoluteIntensityBase(intensity*2.0);
				note->setRelativeHarmonicBase(pitch*0.005);
				note->updateSynthParameters();
				break;
			case kKeyStatePress:
				break;
			case kKeyStateDown:
				note = noteForKey(key);
				if(note == NULL)
					break;
				
				if(note->useKeyDownHoldoff())
				{
					if(keyDownVelocity_[key] > (white ? keyDownHoldoffVelocityWhite_ : keyDownHoldoffVelocityBlack_)
					   && framesInCurrentState(key) < note->keyDownHoldoffTime())
					{
						cout << "holdoff time = " << note->keyDownHoldoffTime() << " scaler = " << note->keyDownHoldoffScaler() << endl;
						break;
					}
					
					// If key is held down, make it gradually crescendo
					
					if(keyDownVelocity_[key] > (white ? keyDownHoldoffVelocityWhite_ : keyDownHoldoffVelocityBlack_))
					{
						intensity = framesToSeconds(currentTimeStamp_ - timestampOfStateChange(key, kKeyStateDown))
						* note->keyDownHoldoffScaler();
						if(intensity > 1.0)
							intensity = 1.0;
					}
					else
						intensity = 1.0;		
				}
				else
				{
					//cout << "not using holdoff\n";
					intensity = 1.0;
				}
				
				if(kPianoBarKeyColor[key] == K_B)
				{
					// Black keys are generally not aftertouch-capable, so we fake it by using the nearest white key in the Aftertouch
					// mode.
					
					int nearestWhiteKey = -1;
					
					set<int>::iterator nearestWhiteKeyAbove = activeWhiteKeys_.begin();
					while(nearestWhiteKeyAbove != activeWhiteKeys_.end()){
						if(*nearestWhiteKeyAbove > key)
							break;
						nearestWhiteKeyAbove++;
					}
					
					set<int>::reverse_iterator nearestWhiteKeyBelow = activeWhiteKeys_.rbegin();
					while(nearestWhiteKeyBelow != activeWhiteKeys_.rend()){
						if(*nearestWhiteKeyBelow < key)
							break;
						nearestWhiteKeyBelow++;
					}					
					
					if(nearestWhiteKeyAbove != activeWhiteKeys_.end())
					{
						if(nearestWhiteKeyBelow != activeWhiteKeys_.rend()) // White keys above and below, find closest
						{
							if(*nearestWhiteKeyAbove - key < key - *nearestWhiteKeyBelow)
								nearestWhiteKey = *nearestWhiteKeyAbove;
							else
								nearestWhiteKey = *nearestWhiteKeyBelow;
						}
						else // Nearest white key is above
						{
							nearestWhiteKey = *nearestWhiteKeyAbove;
						}
					}
					else if(nearestWhiteKeyBelow != activeWhiteKeys_.rend())	// Nearest white key is below
					{
						nearestWhiteKey = *nearestWhiteKeyBelow;
					}
					
					// Now check again if we have an associated key.  If so, send its aftertouch messages
					
					if(nearestWhiteKey >= 0 && nearestWhiteKey < 88)
					{
						brightness = ((double)lastPosition(nearestWhiteKey) - 4096.0) / 256.0;
						if(brightness < 0.0)
							brightness = 0.0;
						note->setAbsoluteBrightness(brightness);						
					}
				}				

				note->setAbsoluteIntensityBase(intensity);
				note->updateSynthParameters();

				break;
			case kKeyStateAftertouch:
				note = noteForKey(key);
				if(note == NULL)
					break;				

				if(note->useKeyDownHoldoff())
				{
					if(keyDownVelocity_[key] > (white ? keyDownHoldoffVelocityWhite_ : keyDownHoldoffVelocityBlack_)
					   && (currentTimeStamp_ - timestampOfStateChange(key, kKeyStateDown)) < note->keyDownHoldoffTime())
					{
						cout << "holdoff time = " << note->keyDownHoldoffTime() << " scaler = " << note->keyDownHoldoffScaler() << endl;
						break;
					}

					// If key is held down, make it gradually crescendo
					
					if(keyDownVelocity_[key] > (white ? keyDownHoldoffVelocityWhite_ : keyDownHoldoffVelocityBlack_))
					{
						intensity = framesToSeconds(currentTimeStamp_ - timestampOfStateChange(key, kKeyStateDown))
						* note->keyDownHoldoffScaler();
						if(intensity > 1.0)
							intensity = 1.0;
					}
					else
						intensity = 1.0;
		
				}
				else
					intensity = 1.0;
				
				note->setAbsoluteIntensityBase(intensity);		
				
				//note->setAbsoluteIntensityVibrato(((double)lastPosition(key) - 4096.0) / 256.0);
				brightness = ((double)lastPosition(key) - 4096.0) / 256.0;
				if(brightness < 0.0)
					brightness = 0.0;
				note->setAbsoluteBrightness(brightness);
				
				/*pitch = 0.0;
				
				// Check neighboring keys for pretouch gestures to use for pitch bends
				if(currentState(whiteKeyAbove(key)) == kKeyStatePretouch)
				{
					pos = runningPositionAverage(whiteKeyAbove(key), 0, 6);
					pitch += (double)(pos - keyIdleThreshold_[whiteKeyAbove(key)]) / 4096.0;
				}
				if(currentState(whiteKeyBelow(key)) == kKeyStatePretouch)
				{
					pos = runningPositionAverage(whiteKeyBelow(key), 0, 6);
					pitch -= (double)(pos - keyIdleThreshold_[whiteKeyBelow(key)]) / 4096.0;						
				}			
				note->setAbsolutePitchVibrato(pitch);*/
				
				note->updateSynthParameters();				
				break;
			case kKeyStateAfterVibrato:
				// Later
				break;
			case kKeyStateRelease:
				break;
			case kKeyStateIdle:			// These states don't send any messages.  Hopefully most keys are
			case kKeyStateUnknown:		// idle at any given time.
			case kKeyStateDisabled:
			default:
				break;
		}
	}	
	
	lastStateMessage_ = currentTimeStamp_;
}

// Look for a pitch-bend action: a key in the Down or Aftertouch position
// whose neighboring white key changed to Pretouch AFTER the key press
// Bend the frequency of the "down" note and give it a richer spectrum,
// while disabling individual sounding for the neighboring note

void PianoBarController::handleMultiKeyPitchBend(set<int> *keysToSkip)
{
	int centerKey, bendUpKey, bendDownKey;
	int centerKeyState;
	double pitch;
	RealTimeMidiNote *note, *auxNote;
	
	for(centerKey = 0; centerKey < 88; centerKey++)
	{
		centerKeyState = currentState(centerKey);
		if(centerKeyState != kKeyStateDown && centerKeyState != kKeyStateAftertouch && centerKeyState != kKeyStateAfterVibrato)
			continue;
		note = noteForKey(centerKey);
		if(note == NULL)
			continue;
		if(note->harmonicSweepRange() != 0)	// Pitch bend not enabled by default on harmonic sweep mode
		{
			if(!note->usePitchBendWithHarmonics())
				continue;
			if(note->harmonicSweepRange() > 0)
			{
				bendUpKey = whiteKeyBelow(centerKey);
				bendDownKey = whiteKeyBelow(whiteKeyBelow(centerKey));
			}
			else
			{
				bendDownKey = whiteKeyAbove(centerKey);
				bendUpKey = whiteKeyAbove(whiteKeyAbove(centerKey));				
			}
		}
		else
		{
			bendUpKey = whiteKeyAbove(centerKey);
			bendDownKey = whiteKeyBelow(centerKey);
		}
		
		pitch = 0.0;
		
		if(currentState(bendUpKey) == kKeyStatePretouch || currentState(bendUpKey) == kKeyStatePreVibrato)
		{
			// Check that the Pretouch action happened after the center key Down action
			if(timestampOfStateChange(centerKey, kKeyStateDown) < timestampOfStateChange(bendUpKey, kKeyStatePretouch))
			{
				int pos = runningPositionAverage(bendUpKey, 0, 6);
				double pitchUp = (double)(pos - keyIdleThreshold_[bendUpKey]) / 4096.0;
				pitch += pitchUp;	
				
				auxNote = noteForKey(bendUpKey);
				if(auxNote != NULL)
				{
					/*auxNote->setAbsoluteIntensityBase(0.0);
					auxNote->updateSynthParameters();
					keysToSkip->insert(bendUpKey);*/
					auxNote->setAbsolutePitchVibrato(-1.0 + pitchUp);	// TESTME: connecting pitch bends
					auxNote->updateSynthParameters();
				}
			}
		}
		if(currentState(bendDownKey) == kKeyStatePretouch || currentState(bendDownKey) == kKeyStatePreVibrato)
		{
			if(timestampOfStateChange(centerKey, kKeyStateDown) < timestampOfStateChange(bendDownKey, kKeyStatePretouch))
			{
				int pos = runningPositionAverage(bendDownKey, 0, 6);
				double pitchDown = (double)(pos - keyIdleThreshold_[bendDownKey]) / 4096.0;
				pitch -= pitchDown;

				auxNote = noteForKey(bendDownKey);
				if(auxNote != NULL)
				{
					/*auxNote->setAbsoluteIntensityBase(0.0);
					auxNote->updateSynthParameters();
					keysToSkip->insert(bendDownKey);*/
					auxNote->setAbsolutePitchVibrato(1.0 - pitchDown);
					auxNote->updateSynthParameters();					
				}				
			}
		}
		
		//note->setAbsoluteIntensityBase(0.9 + pitch/2.0);
		note->setAbsolutePitchVibrato(pitch);
		note->updateSynthParameters();	
	}
}

// Look for a pretouch action one octave above a held note-- use it to sweep up and down the
// harmonic series of the center note

void PianoBarController::handleMultiKeyHarmonicSweep(set<int> *keysToSkip)
{
	int centerKey, centerKeyState, harmonic, currentKey, range, spread, adjustment;
	bool up, foundHarmonics;
	RealTimeMidiNote *note, *auxNote;
	vector<double> harmonicValues;
	
	for(centerKey = 0; centerKey < 88; centerKey++)
	{
		centerKeyState = currentState(centerKey);
		if(centerKeyState != kKeyStateDown && centerKeyState != kKeyStateAftertouch && centerKeyState != kKeyStateAfterVibrato)
			continue;
		note = noteForKey(centerKey);
		if(note == NULL)
			continue;
		if(note->harmonicSweepRange() == 0)	// If this parameter isn't enabled, ignore this note
			continue;
		
		range = note->harmonicSweepRange();
		up = true;
		if(range < 0)
			up = false;
		range = abs(range);
		spread = note->harmonicSweepSpread();		
			
		foundHarmonics = false;
		harmonicValues.clear();
		
		adjustment = 0;
		
		if(spread != 0)
		{
			// Figure out how many other keys in range are acting as fundamentals, and adjust the effect of the upper keys accordingly
			
			harmonic = 1;
			currentKey = up ? whiteKeyAbove(centerKey) : whiteKeyBelow(centerKey);
			
			while(harmonic <= range)
			{
				// Requirements: target key is down, and either target key was tapped or target key went down before this one.
				
				if((currentState(currentKey) == kKeyStateDown || currentState(currentKey) == kKeyStateAftertouch || currentState(currentKey) == kKeyStateAfterVibrato)
				   && ((timestampOfStateChange(currentKey, kKeyStateTap) > timestampOfStateChange(currentKey, kKeyStateIdle)
					   && timestampOfStateChange(currentKey, kKeyStateTap) > timestampOfStateChange(currentKey, kKeyStateRelease))
					   || timestampOfStateChange(centerKey, kKeyStateDown) >= timestampOfStateChange(currentKey, kKeyStatePretouch)))
				{
					adjustment += spread;
				}
				
				//currentKey = up ? whiteKeyAbove(currentKey) : whiteKeyBelow(currentKey);
				currentKey = up ? currentKey + 1 : currentKey - 1;
				if(kPianoBarKeyColor[currentKey] == K_W)
					harmonic++;
				if(currentKey >= 88 || currentKey < 0)
					break;				
			}
		}
		
		harmonic = 1;
	    currentKey = up ? whiteKeyAbove(centerKey) : whiteKeyBelow(centerKey);
				   
		if(adjustment > 0)
			for(int i = 0; i < adjustment; i++)		// Offset the harmonics if necessary
				harmonicValues.push_back(0.0);
		
		while(harmonic <= range)	// Check all the keys within the range, or until we reach the end of the keyboard
		{
			if(harmonic + adjustment <= 0)	// If the spread between keys makes this key irrelevant, ignore it
			{
				harmonic++;
				currentKey = up ? whiteKeyAbove(currentKey) : whiteKeyBelow(currentKey);
				if(currentKey >= 88 || currentKey < 0)
					break;				
				continue;
			}
			
			// Keys that were tapped should count as fundamentals, not as harmonics.  Tap state comes between Idle and Pretouch.

			if(!(timestampOfStateChange(currentKey, kKeyStateTap) > timestampOfStateChange(currentKey, kKeyStateIdle) &&
			   timestampOfStateChange(currentKey, kKeyStateTap) > timestampOfStateChange(currentKey, kKeyStateRelease))
			   && currentState(currentKey) != kKeyStateIdle && currentState(currentKey) != kKeyStateDisabled && currentState(currentKey) != kKeyStateUnknown)
			{
				// Check that the Pretouch action happened after the center key Down action
				if(timestampOfStateChange(centerKey, kKeyStateDown) < timestampOfStateChange(currentKey, kKeyStatePretouch))
				{
					int pos = runningPositionAverage(currentKey, 0, 6);
					double value = (double)(pos - keyIdleThreshold_[currentKey]) / 3072.0;	
					
					auxNote = noteForKey(currentKey);
					if(auxNote != NULL)
					{
						//auxNote->setAbsoluteIntensityBase(0.0);
						//auxNote->updateSynthParameters();
						auxNote->abort();	// FIXME: does this work?
						keysToSkip->insert(currentKey);
					}					
					
					harmonicValues.push_back(value);
					foundHarmonics = true;
				}
				else
					harmonicValues.push_back(0.0);

			}
			else
				harmonicValues.push_back(0.0);
			
			harmonic++;
			currentKey = up ? whiteKeyAbove(currentKey) : whiteKeyBelow(currentKey);
			if(currentKey >= 88 || currentKey < 0)
				break;
		}
		
		//if(foundHarmonics)
			note->setRawHarmonicValues(harmonicValues);
	}
	
	/*int centerKey, octaveAbove;
	int centerKeyState;
	double harmonic;
	RealTimeMidiNote *note, *auxNote;
	
	for(centerKey = 0; centerKey < 76; centerKey++)	// This doesn't make sense for the top octave of keys
	{
		centerKeyState = currentState(centerKey);
		if(centerKeyState != kKeyStateDown && centerKeyState != kKeyStateAftertouch && centerKeyState != kKeyStateAfterVibrato)
			continue;
		note = noteForKey(centerKey);
		if(note == NULL)
			continue;
		
		octaveAbove = centerKey + 12;
		if(kPianoBarKeyColor[octaveAbove] != K_W)	// Use a major seventh above in the case of black keys
			octaveAbove--;

		harmonic = 0.0;
		
		if(currentState(octaveAbove) == kKeyStatePretouch || currentState(octaveAbove) == kKeyStatePreVibrato)
		{
			// Check that the Pretouch action happened after the center key Down action
			if(timestampOfStateChange(centerKey, kKeyStateDown) < timestampOfStateChange(octaveAbove, kKeyStatePretouch))
			{
				int pos = runningPositionAverage(octaveAbove, 0, 6);
				harmonic += (double)(pos - keyIdleThreshold_[octaveAbove]) / 2048.0;	
				
				auxNote = noteForKey(octaveAbove);
				if(auxNote != NULL)
				{
					auxNote->setAbsoluteIntensityBase(0.0);
					auxNote->updateSynthParameters();
					keysToSkip->insert(octaveAbove);
				}
			}
			
			note->setAbsoluteHarmonicBase(harmonic);
			note->updateSynthParameters();	
		}
	}	*/
}

// Return the current state of a given key

int PianoBarController::currentState(int key)
{
	if(keyStateHistory_[key].size() == 0)
		return kKeyStateUnknown;
	return keyStateHistory_[key].back().state;
}

// Return the previous state of a given key

int PianoBarController::previousState(int key)
{
	deque<stateHistory>::reverse_iterator rit;
	
	if(keyStateHistory_[key].size() < 2)
		return kKeyStateUnknown;
	
	rit = keyStateHistory_[key].rbegin();
	return (++rit)->state;
}

// Find out how long (in audio frames) the key has been in its current state

pb_timestamp PianoBarController::framesInCurrentState(int key)
{
	pb_timestamp lastTimestamp;
	
	if(keyStateHistory_[key].size() < 2)
		return 0;
	
	deque<stateHistory>::reverse_iterator rit;
	rit = keyStateHistory_[key].rbegin();
	lastTimestamp = (++rit)->timestamp;	
	
	return (currentTimeStamp_ - lastTimestamp);
}

// Find the timestamp of when the key last changed to the indicated state
// Returns 0 if state not found

pb_timestamp PianoBarController::timestampOfStateChange(int key, int state)
{
	deque<stateHistory>::reverse_iterator rit;
	
	rit = keyStateHistory_[key].rbegin();
	
	while(rit != keyStateHistory_[key].rend())
	{
		if(rit->state == state)
			return rit->timestamp;
		rit++;
	}
	
	return 0;
}

#ifdef DEBUG_STATES

const char *kKeyStateNames[kKeyStatesLength] = {"Unknown", "Idle", "Pretouch", "PreVibrato",
	"Tap", "Press", "Down", "Aftertouch", "AfterVibrato", "Release", "Disabled"};

#endif

void PianoBarController::changeKeyStateWithTimestamp(int key, int newState, pb_timestamp timestamp)
{
#ifdef DEBUG_STATES
	cout << "Key " << key << ": " << kKeyStateNames[currentState(key)] << " --> " << kKeyStateNames[newState];
	cout << "        (pos " << lastPosition(key) << ")\n";
#endif
	
	stateHistory st;
	int prevState = currentState(key);
	
	st.state = newState;
	st.timestamp = timestamp;
	
	keyStateHistory_[key].push_back(st);
	while(keyStateHistory_[key].size() > STATE_HISTORY_LENGTH)
		keyStateHistory_[key].pop_front();
	
	keyStateStuckCounter_[key] = 0;
	
	// Going to/from Idle involves starting or stopping a MidiNote.
	
	if(prevState == kKeyStateIdle && newState != kKeyStateIdle)
	{
		vector<unsigned char> message;
		
		message.push_back(MidiController::MESSAGE_NOTEON | midiChannel_);
		message.push_back(key + 21);					// MIDI note number
		message.push_back(0x7F);						// Velocity 127
		
		midiController_->noteOn(0.0, &message, 0);
	}
	else if(prevState != kKeyStateIdle && newState == kKeyStateIdle)
	{
		vector<unsigned char> message;
		
		message.push_back(MidiController::MESSAGE_NOTEOFF | midiChannel_);
		message.push_back(key + 21);					// MIDI note number
		message.push_back(0x7F);						// Velocity 127
		
		midiController_->noteOff(0.0, &message, 0);		
		
		/*if(kPianoBarKeyColor[key] == K_B)
		{
			nearestActiveWhiteKey_[key] = -1;
		}		
		else 
		{
			if(nearestActiveWhiteKey_[key] >= 0 && nearestActiveWhiteKey_[key] < 88 && nearestActiveWhiteKey_[key] != key)
			{
				// Remove this key from the list of attached black keys
				// This shouldn't really come up, since a coupled black key will only cut off with its associated white key
				attachedBlackKeys_[nearestActiveWhiteKey_[key]].erase(key);
				cout << "Removed white key " << nearestActiveWhiteKey_[key] << " from key " << key << endl;
			}
			
			cout << "Black key " << key << " went idle\n";
			nearestActiveWhiteKey_[key] = -1;
		}*/

	}
	
	// When a key goes down (as opposed to merely active), check whether it should trigger a program change
	
	if(newState == kKeyStateDown && prevState != kKeyStateDown)
	{
		midiController_->checkForProgramUpdate(midiChannel_, key + 21);
	}
	
	// Check for white keys in the aftertouch state, which we use to fake aftertouch on the nearby black keys.
	
	if(prevState != kKeyStateAftertouch && prevState != kKeyStateAfterVibrato && (newState == kKeyStateAftertouch || newState == kKeyStateAfterVibrato) && kPianoBarKeyColor[key] == K_W)
	{
		activeWhiteKeys_.insert(key);
		cout << "Added white key " << key << endl;
	}		
	else if(newState != kKeyStateAftertouch && newState != kKeyStateAfterVibrato && (prevState == kKeyStateAftertouch || prevState == kKeyStateAfterVibrato) && kPianoBarKeyColor[key] == K_W)
	{
		/*set<int>::iterator it = attachedBlackKeys_[key].begin();
		while(it != attachedBlackKeys_[key].end())
		{
			// Any attached notes should be reset to no aftertouch
			
			RealTimeMidiNote *note = noteForKey(*it);
			if(note != NULL)
				note->setAbsoluteBrightness(0);	
			//cout << "Aborting note " << *it << " attached to key " << key << endl;
			
			nearestActiveWhiteKey_[*it++] = -1;	// Go back to looking for a key
		}*/
		
		//attachedBlackKeys_[key].clear();
		activeWhiteKeys_.erase(key);
		cout << "White key " << key << " went idle\n";		
	}
}

// Reset all key states to Unknown

void PianoBarController::resetKeyStates()
{
	for(int i = 0; i < 88; i++)
	{
		stateHistory sh;
		
		keyStateHistory_[i].clear();
		
		sh.state = kKeyStateUnknown;
		sh.timestamp = currentTimeStamp_;
		keyStateHistory_[i].push_back(sh);
		
		//nearestActiveWhiteKey_[i] = -1;
		//attachedBlackKeys_[i].clear();
	}
	
	activeWhiteKeys_.clear();
}

// Return true only at specified intervals to limit the speed of debugging print messages

bool PianoBarController::debugPrintGate(int key, pb_timestamp delay)
{
	if(currentTimeStamp_ > debugLastPrintTimestamp_[key] + delay)
	{
		debugLastPrintTimestamp_[key] = currentTimeStamp_;
		return true;
	}
	return false;
}

#pragma mark Quiescent State Estimation

// Do all the memory allocation necessary for the key Idle state model, so we don't have to do it
// repeatedly several times per second!

void PianoBarController::initializeKeyQuiescentModel()
{
/*	idleStateLinearWorkspace_ = gsl_multifit_linear_alloc(NUM_WHITE_KEYS, 3);	
	idleStateX_ = gsl_matrix_alloc(NUM_WHITE_KEYS, 3);
	idleStateY_ = gsl_vector_alloc(NUM_WHITE_KEYS);
	idleStateW_ = gsl_vector_alloc(NUM_WHITE_KEYS);
	idleStateCovariance_ = gsl_matrix_alloc(3,3);
	idleStateCoefficients_ = gsl_vector_alloc(3);
*/
}

void PianoBarController::freeKeyQuiescentModel()
{
/*
	gsl_multifit_linear_free(idleStateLinearWorkspace_);	
	gsl_matrix_free(idleStateX_);
	gsl_vector_free(idleStateY_);
	gsl_vector_free(idleStateW_);
	gsl_matrix_free(idleStateCovariance_);
	gsl_vector_free(idleStateCoefficients_);
*/
}

void PianoBarController::updateKeyQuiescentModel()
{
/*	
	// In this function, calculate a quadratic best fit for all white keys that are not currently in "press" mode.
	// Use this later to set a dynamic threshold for keys pressed.  Black keys don't have the same mechanical crosstalk
	// problems so we exclude them here.
	
	int i, j, pos, weight, keyState;
	double chisq;
	//gsl_matrix *cov;
	//cov = gsl_matrix_alloc (3, 3);
	
	j = 0;
	pos = 0;
	
	for (i = 0; i < 88; i++)
	{
		if(kPianoBarKeyColor[i] != K_W)		// Only look at the white keys
			continue;
		
		keyState = currentState(i);
		
		if(keyState == kKeyStateIdle || keyState == kKeyStatePretouch || keyState == kKeyStatePreVibrato)
		{
			pos = lastPosition(i);
			if(pos > -100)
				weight = 1.0;
			else if(pos < -1000)
				weight = 0.0;
			else
				weight = (double)(pos + 1000) / 900.0;
			
			//if(keyState != kKeyStateIdle)		// Include supposedly "pretouch" keys in the mix, but with less weight
			//	weight *= 0.5;
			
			gsl_vector_set(idleStateW_, j, weight);
		}
		else
			gsl_vector_set(idleStateW_, j, 0.0);	// Give zero weight to non-idle keys
		
		gsl_matrix_set (idleStateX_, j, 0, 1.0);
		gsl_matrix_set (idleStateX_, j, 1, (double)i);
		gsl_matrix_set (idleStateX_, j, 2, (double)(i*i));
		
		gsl_vector_set (idleStateY_, j, pos);
		
		j++;
	}

	gsl_multifit_wlinear (idleStateX_, idleStateW_, idleStateY_, idleStateCoefficients_, idleStateCovariance_,
						  &chisq, idleStateLinearWorkspace_);

	// DEBUG
	//printf ("# best fit: Y = %g + %g X + %g X^2\n", 
	//		gsl_vector_get(idleStateCoefficients_, 0), gsl_vector_get(idleStateCoefficients_, 1),
	//		gsl_vector_get(idleStateCoefficients_, 2));
*/
}

// Return the value that the best-fit model predicts for the quiescent value of this key.

int PianoBarController::predictedQuiescentValue(int key)
{
	double result = 0.0;
	
	//result = gsl_vector_get(idleStateCoefficients_, 0) + gsl_vector_get(idleStateCoefficients_, 1)*(double)key
	//		+ gsl_vector_get(idleStateCoefficients_, 2)*(double)(key*key);
	
	return (int)result;
}

