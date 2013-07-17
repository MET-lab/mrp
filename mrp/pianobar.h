/*
 *  pianobar.h
 *  mrp
 *
 *  Created by Andrew McPherson on 2/10/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef PIANOBAR_H
#define PIANOBAR_H

#include <iostream>
#include <cmath>
#include <vector>
#include <deque>
#include <fstream>
#include <pthread.h>
#include <unistd.h>
//#include <gsl/gsl_multifit.h>
#include "portaudio.h"
#include "config.h"
#ifdef __APPLE__
#include "pa_mac_core.h"
#endif
#include "midicontroller.h"
#include "realtimenote.h"


using namespace std;

#define DEBUG_CALIBRATION
#define DEBUG_STATES

#define PIANO_BAR_SAMPLE_RATE (10700)
#define PB_MIDICONTROLLER_CHANNEL (0x0F)

// Mapping of data bins to MIDI note numbers for Moog Piano Bar.  Left to right represents pads "GRP1" to "GRP12" on
// scanner bar.  Top to bottom represents 18 successive values after a sync pulse.

const short kPianoBarMapping[18][12] = {						
	{24, 26, 41, 43, 52, 53, 69, 71, 81, 83, 98, 100},		
	{31, 33, 0,  0,  59, 60, 76, 0,  88, 89, 105, 107},
	{22, 25, 37, 39, 49, 51, 63, 66, 78, 80, 92, 94},
	{32, 34, 46, 0,  58, 61, 73, 75, 87, 90, 102, 104},
	{27, 30, 42, 44, 54, 56, 68, 70, 82, 85, 97, 99},
	{35, 36, 0,  0,  62, 64, 0,  0,  91, 93, 106, 108},
	{21, 23, 38, 40, 48, 50, 65, 67, 81, 83, 98, 100},
	{35, 36, 0,  0,  62, 64, 0,  0,  91, 93, 106, 108},
	{22, 25, 37, 39, 49, 51, 63, 66, 78, 80, 92, 94},
	{32, 34, 46, 0,  58, 61, 73, 75, 87, 90, 102, 104},
	{27, 30, 42, 44, 54, 56, 68, 70, 82, 85, 97, 99},	
	{35, 36, 0,  0,  62, 64, 0,  0,  91, 93, 106, 108},	
	{21, 23, 38, 40, 48, 50, 65, 67, 77, 79, 95, 96},
	{28, 29, 47, 45, 55, 57, 72, 74, 84, 86, 101, 103},
	{22, 25, 37, 39, 49, 51, 63, 66, 78, 80, 92, 94},
	{32, 34, 46, 0,  58, 61, 73, 75, 87, 90, 102, 104},
	{27, 30, 42, 44, 54, 56, 68, 70, 82, 85, 97, 99},
	{35, 36, 0,  0,  62, 64, 0,  0,  91, 93, 106, 108}};

// Signal identities: the white keys and black keys on the Piano Bar operate
// differently (white = reflectance, black = breakbeam).  What's more, those
// keys that repeat within a cycle may not have the same exact lighting amplitude
// each time, for which we may need to compensate.

enum {
	PB_NA = 0,		// Unknown / blank (not all signal bins in PB data represent actual keys)
	PB_W1 = 1,		// White keys
	PB_W2 = 2,
	PB_W3 = 3,
	PB_W4 = 4,
	PB_B1 = 5,		// Black keys
	PB_B2 = 6,
	PB_B3 = 7,
	PB_B4 = 8
};

enum {				// Key color
	K_W = 0,
	K_B = 1
};

const short kPianoBarSignalTypes[18][12] = {
	{PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1},
	{PB_W1, PB_W1, PB_NA, PB_NA, PB_W1, PB_W1, PB_W1, PB_NA, PB_W1, PB_W1, PB_W1, PB_W1},
	{PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1},
	{PB_B1, PB_B1, PB_B1, PB_NA, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1},
	{PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1, PB_B1},
	{PB_W1, PB_W1, PB_NA, PB_NA, PB_W1, PB_W1, PB_NA, PB_NA, PB_W1, PB_W1, PB_B1, PB_W1},
	{PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W2, PB_W2, PB_W2, PB_W2},
	{PB_W2, PB_W2, PB_NA, PB_NA, PB_W2, PB_W2, PB_NA, PB_NA, PB_W2, PB_W2, PB_B2, PB_W2},
	{PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2},
	{PB_B2, PB_B2, PB_B2, PB_NA, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2},
	{PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2, PB_B2},
	{PB_W3, PB_W3, PB_NA, PB_NA, PB_W3, PB_W3, PB_NA, PB_NA, PB_W3, PB_W3, PB_B3, PB_W3},
	{PB_W2, PB_W2, PB_W2, PB_W2, PB_W2, PB_W2, PB_W2, PB_W2, PB_W1, PB_W1, PB_W1, PB_W1},
	{PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1, PB_W1},
	{PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3},
	{PB_B3, PB_B3, PB_B3, PB_NA, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3},
	{PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3, PB_B3},
	{PB_W4, PB_W4, PB_NA, PB_NA, PB_W4, PB_W4, PB_NA, PB_NA, PB_W4, PB_W4, PB_B4, PB_W4}};	

const short kPianoBarKeyColor[88] = {			 K_W, K_B, K_W,		// Octave 0
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 1
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 2
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 3
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 4
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 5
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 6
	K_W, K_B, K_W, K_B, K_W, K_W, K_B, K_W, K_B, K_W, K_B, K_W,		// Octave 7
	K_W };															// Octave 8

#define NUM_WHITE_KEYS 52
#define NUM_BLACK_KEYS 36

// Calibration status of the Piano Bar.  This compensates for variations in mechanical position, light level, etc.
enum {
	kPianoBarNotCalibrated = 0,
	kPianoBarCalibrated,
	kPianoBarInCalibration
};

// Possible states for each key to be in.  Conventional MIDI representations only admit Idle, Press, Down, and Release
// (and sometimes Aftertouch) but we can track a variety of extended keyboard gestures.

enum {
	kKeyStateUnknown = 0,		// Will be in unknown state before calibration
	kKeyStateIdle,
	kKeyStatePretouch,
	kKeyStatePreVibrato,
	kKeyStateTap,
	kKeyStatePress,
	kKeyStateDown,
	kKeyStateAftertouch,
	kKeyStateAfterVibrato,
	kKeyStateRelease,
	kKeyStateDisabled,
	kKeyStatesLength
};

#define UNCALIBRATED (short)(0x7FFF)		// Initial value for calibration settings
#define MIN_KEY_PRESSURE_DIFF 12			// Minimum difference between heavy and light touches to discriminate
											// key pressure (aftertouch)
#define VELOCITY_SCALER 64					// Amount by which we mulitply velocity results for better (int) resolution
#define STATE_HISTORY_LENGTH 8				// How many old states to save for each key

typedef unsigned long long pb_timestamp;
typedef long long pb_time_offset;

class PianoBarController
{
public:
	PianoBarController(MidiController *midiController) : isInitialized_(false), isRunning_(false), inputStream_(NULL),
														 midiChannel_(PB_MIDICONTROLLER_CHANNEL) {
		for(int i = 0; i < 88; i++)
			keyHistory_[i] = NULL;
		pthread_mutex_init(&audioMutex_, NULL);
		midiController_ = midiController;
		initializeKeyQuiescentModel();
	}
	
	bool open(PaDeviceIndex inputDeviceNum, int bufferSize, float historyInSeconds);
	bool start();		// Start, stop, and close control the stream supplied by open.  start and stop begin
	bool stop();		// and end data capture, where close releases everything allocated by open
	void close();		// These return true on success.
	
	void setMidiChannel(int newChannel) { midiChannel_ = newChannel; } // Set the channel we send MidiController messages to
	
	bool startCalibration(vector<int> &keysToCalibrate, bool quiescentOnly);	// Call these after the device is running.  Calibrate specific PB data.
	void stopCalibration();
	bool saveCalibrationToFile(string& filename);	// Save calibration settings to a file
	bool loadCalibrationFromFile(string& filename);	// Load calibration settings from a file
	bool isCalibrating() { return (calibrationStatus_ == kPianoBarInCalibration); }
	bool isInitialized() { return isInitialized_; }
	bool isRunning() { return isRunning_; }
	
	void printKeyStatus();									// Print the current status of each key
	
	
	static int staticAudioCallback(const void *input, void *output, unsigned long frameCount, 
								   const PaStreamCallbackTimeInfo* timeInfo,
								   PaStreamCallbackFlags statusFlags, void *userData) 
	{
		return ((PianoBarController *)userData)->audioCallback(input, output, frameCount, timeInfo, statusFlags);
	}	
	
	~PianoBarController();
	
private:
	void printKeyStatusHelper(int start, int length, int padSpaces);	// Helper for printKeyStatus()
	
	// audioCallback() does the real heavy lifting.  staticAudioCallback is just there to provide a hook
	// for portaudio into this object.
	
	int audioCallback(const void *input, void *output,
					  unsigned long frameCount,
					  const PaStreamCallbackTimeInfo* timeInfo,
					  PaStreamCallbackFlags statusFlags);
	void processValue(short midiNote, short type, short value);
	
	int lastPosition(int key) { return keyHistory_[key][keyHistoryPosition_[key]]; }
	int runningPositionAverage(int key, int offset, int length);
	int runningVelocityAverage(int key, int offset, int length);
	int runningAccelerationAverage(int key, int offset, int length);
	int peakAcceleration(int key, int offset, int distanceToSearch, int samplesToAverage, bool positive);
	
	int calibrationRunningAverage(int key, int seq, int offset, int length);
	void updateKeyStates();
	void sendKeyStateMessages();
	
	void handleMultiKeyPitchBend(set<int> *keysToSkip);
	void handleMultiKeyHarmonicSweep(set<int> *keysToSkip);
	
	int currentState(int key);								// Return current key state
	int previousState(int key);								// Return key state before this one
	pb_timestamp framesInCurrentState(int key);		// How long have we been in the current state?
	pb_timestamp timestampOfStateChange(int key, int state);
	pb_timestamp secondsToFrames(double seconds) {	// Useful conversion methods
		return (pb_timestamp)(seconds * (double)PIANO_BAR_SAMPLE_RATE);
	}
	double framesToSeconds(pb_timestamp frames) {
		return ((double)frames / (double)PIANO_BAR_SAMPLE_RATE);
	}
	pb_timestamp timestampForOffset(int key, int offset) {	// Return the timestamp of a previous sample
		if(key < 0 || key > 87)
			return 0;
		int loc = (keyHistoryPosition_[key] - offset + keyHistoryLength_[key]) % keyHistoryLength_[key];
		return keyHistoryTimestamps_[key][loc];
	}
	int timestampToKeyOffset(int key, pb_timestamp timestamp) {	// How many samples ago was this timestamp?
		if(key < 0 || key > 87)
			return 0;
		int loc = keyHistoryPosition_[key], count = 0;
		while(keyHistoryTimestamps_[key][loc] > timestamp)			// TODO: pre-estimate...
		{
			loc = (loc - 1 + keyHistoryLength_[key]) % keyHistoryLength_[key];
			count++;
			
			if(count >= keyHistoryLength_[key])	// Hopefully nobody asks for something this big!
				return 0;
		}
		return count;
	}
	RealTimeMidiNote *noteForKey(int key) {		// Return the MIDI note for a given key, if it exists
		unsigned int lookupIndex = ((unsigned int)(midiChannel_) << 8) + (unsigned int)(key + 21);
		if(midiController_->currentNotes_.count(lookupIndex) == 0)
			return NULL;
		Note *note = midiController_->currentNotes_[lookupIndex];
		if(typeid(*note) != typeid(RealTimeMidiNote))
			return NULL;
		return (RealTimeMidiNote *)note;
	}

	bool debugPrintGate(int key, pb_timestamp delay);	// Method that tells us whether to dump something to the console
	
	void changeKeyStateWithTimestamp(int key, int newState, pb_timestamp timestamp);
	void changeKeyState(int key, int state) { changeKeyStateWithTimestamp(key, state, currentTimeStamp_); }
	bool cleanUpCalibrationValues();
	void resetKeyStates();
	
	int whiteKeyAbove(int key) {		// Return the index of the next white key above this key
		if(key >= 87) return 0;
		if(kPianoBarKeyColor[key+1] == K_B)
			return key+2;
		return key+1;
	}
	int whiteKeyBelow(int key) {		// Return the index of the next white key below this one
		if(key <= 0) return 87;	// "circular" keyboard! (have to return SOMETHING for the bottom key)
		if(kPianoBarKeyColor[key-1] == K_B)
			return key-2;
		return key-1;
	}
	
	// Quiescent state modeling
	
	void initializeKeyQuiescentModel();
	void freeKeyQuiescentModel();
	void updateKeyQuiescentModel();
	int predictedQuiescentValue(int key);
	
	bool isInitialized_;			// Whether the audio device has been initialized
	bool isRunning_;				// Whether the device is currently capturing data
	int bufferSize_;
	int midiChannel_;				// Channel on which we broadcast messages to MidiController
	PaStream *inputStream_;			// Reference to the audio stream
	pthread_mutex_t audioMutex_;	// Mutex to synchronize between audio callbacks and user function calls
	MidiController *midiController_; // Reference to the controller which handles synth note allocation
	volatile pb_timestamp currentTimeStamp_;	// Current time in ADC frames (10.7kHz), relative to device start
	
	// GSL Library variables for performing best fit Idle state estimation
	
	//gsl_multifit_linear_workspace *idleStateLinearWorkspace_;		// Workspace for finding best-fit models of Idle state
	//gsl_matrix *idleStateX_, *idleStateCovariance_;
	//gsl_vector *idleStateY_, *idleStateW_, *idleStateCoefficients_;
	
	// Calibration Info
	
	int calibrationStatus_;			// Whether the Piano Bar has been calibrated or not
	
	// For right now, we store buffers that are larger than technically necessary: we only use 1 cycle point
	// per white key, 3 per black key, but conceivably we might want to use up to 4 cycle points for keys that support it.
	
	short calibrationQuiescent_[88][4];			// Resting state of each key
	short calibrationLightPress_[88][4];		// Lightest "pressed" state for each key
	short calibrationHeavyPress_[88][4];		// Heaviest "pressed" state for each key
	bool calibrationOkToCalibrateLight_[88][4];		// Whether we're in the middle of a key press or not-- need this to
												// catch only the first instance of a "light" key press
	bool calibrationCanReadKeyPressure_[88];	// Whether or not there's a substantial difference in reading between
												// light and heavy key pressure-- prevents divide by 0 and spurious data
		
	short *calibrationHistory_[88][4];			// Special buffer allocated only during calibration time which keeps
												// each sample within a sequence separate
	int calibrationHistoryPosition_[88][4];		// Position within each buffer
	int calibrationHistoryLength_;				// Same length for each buffer
	vector<int> *keysToCalibrate_;				// Which keys we're currently calibrating (NULL for all)
	volatile unsigned int calibrationSamples_;			// How many raw samples we've taken
	
	// History of each key position
	
	int *keyHistory_[88];							// History of each key position, allocated dynamically
	pb_timestamp *keyHistoryTimestamps_[88];	// Specific time stamps for each point in the key history
	int keyHistoryLength_[88];					// History length of each specific key
	int keyHistoryPosition_[88];				// Where we are within each buffer

	pb_timestamp debugLastPrintTimestamp_[88];	// For debugging purposes, the last time a message was printed
	
	// State information
	
	typedef struct {
		int state;								// see enum { kKeyState... } above
		pb_timestamp timestamp;			// Timestamp measured in ADC samples [10.7kHz]
	} stateHistory;
	
	deque<stateHistory> keyStateHistory_[88];	// History of each key state
	
	pb_timestamp lastStateUpdate_;		// When we last updated the state values (even if no state changed)
	pb_timestamp lastStateMessage_;		// When we last sent key messages (might be the same or different as above)
	
	//int keyStateCurrent_[88];					// State of each key (see definitions at top)
	//int keyStatePrevious_[88];					// The last state the key was in-- sometimes affects current state
	//int keyStateFrameCounter_[88];				// How many cycles we've been in the current state
	int keyStateStuckCounter_[88];				// How long this key has been "stuck" in Pretouch mode
	int keyDownInitialPosition_[88];			// What the initial key-down value was
	int keyDownVelocity_[88];					// Velocity of key strike (for Down/Aftertouch modes)
	int keyLastPosition_[88];					// Last position of this key, for "sticking" purposes
	int keyVibratoCount_[88];					// How many vibrato cycles have taken place
	pb_timestamp keyLastVibratoTimestamp_[88];				// Last frame count when vibrato took place

	int keyIdleThreshold_[88];					// Position [0-4096] below which we can assume idle
	
	int keyPressPositionThreshold_;				// Position above which we trigger a press motion regardless of velocity
	int keyPressVelocityThreshold_;
	int keyPretouchIdleVelocity_;					// Velocity below which (in conjunction with position) key is idle
	int keyDownVelocityThreshold_;				// Velocity below which a press ends and "down" state begins
	int keyReleasePositionThreshold_;			// Position below which release begins
	pb_timestamp keyIdleBounceTime_;			// Time after release when the key is bouncing
	pb_timestamp keyDownBounceTime_;			// Time alloted for key bouncing after press
	int keyAftertouchThreshold_;				// Distance from initial "down" position to suggest aftertouch
	int keyStuckPositionThreshold_;				// Difference from last position to this one to indicate a key in stasis
	int keyTapAccelerationThreshold_;			// (Positive) acceleration value indicating tap
	int keyPreVibratoVelocity_;					// Positive and negative velocity to indicate a vibrato action
	pb_timestamp keyPreVibratoTimeout_;			// Number of frames to go by before we declare the vibrato gesture over
	pb_timestamp keyPreVibratoFrameSpacing_;	// How many frames have to elapse before a new vibrato action can occur
	int keyPretouchHoldoffVelocity_;			// Velocity above which we wait to actually generate sound...
	int keyDownHoldoffVelocityWhite_;			// Threshold to define a hammer-strike (vs. silent) press
	int keyDownHoldoffVelocityBlack_;
	pb_timestamp keyDownHoldoffTime_;			// How long to wait before engaging resonator after hammer strike
	pb_timestamp multikeySweepMaxSpacing_;		// How long between key events we 
	
	// Variables to simulate continuous key position on the black keys
	//int nearestActiveWhiteKey_[88];					// For each key, the number of the nearest non-idle white key
	set<int> activeWhiteKeys_;						// which white keys are active
	//set<int> attachedBlackKeys_[88];				// What black keys are listening to a given white key
};

#endif