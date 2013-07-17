/*
 *  midicontroller.h
 *  mrp
 *
 *  Created by Andrew McPherson on 10/26/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef MIDICONTROLLER_H
#define MIDICONTROLLER_H

#define DAMPER_PEDAL_THRESHOLD		32		// What MIDI controller value is "depressed" enough for our purposes
#define SOSTENUTO_PEDAL_THRESHOLD	64		// 64 or above = depressed

#define OSC_MIDI_CONTROLLER_NUM		15		// "Port" number of the OSC MIDI emulator

#define DEBUG_MESSAGES
#define DEBUG_MESSAGES_EXTRA

#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <cstring>
#include "tinyxml.h"
#include "RtMidi.h"
#include "audiorender.h"
#include "osccontroller.h"

using namespace std;

class Note;
class PitchTrackController;
class PianoBarController;
class PNOscanController;

class MidiController : public OscHandler
{
	friend class PitchTrackController;
	friend class PianoBarController;
    friend class PNOscanController;
	friend class OscController;
	friend class CalibratorNote;	// Declare this as friend so it can read/write the calibration values
	friend void *cleanupLoop(void *data);
public:
	typedef struct {				// Struct holding some basic data about each MIDI program
		Note *notes[4];				// One of these structs will exist for each key of each channel (up to 16*128)
        // notes[0] => low vel / no aux; notes[1] => high vel / no aux; notes[2, 3] => with aux pedal
		bool useDamperPedal;
		bool useSostenutoPedal;
		bool useAuxPedal;
        bool sustainAlways;
        bool retriggerEachPress;
		int velocitySplitPoint;
		int priority;
        int monoVoice;              // Monophonic voice this note is assigned to (0-15, or -1 for none)
		float amplitudeOffset;      // Volume adjustment
	} ProgramInfo;
	
	// FIXME: Really, we should replace all this program info business with some giant tree structure that splits
	// based on any number of parameters: program #, note, channel, velocity, control values, phase of the moon, etc.
	
	typedef struct {
		MidiController *controller;	// The specific object to which this message should be routed
		RtMidiIn *midiIn;			// The object which actually handles the MIDI input
		int inputNumber;			// An index indicating which input this came from in the user-defined order
	} midiCallbackStruct;
	
	enum {							// MIDI messages
		MESSAGE_NOTEOFF = 0x80,
		MESSAGE_NOTEON = 0x90,
		MESSAGE_AFTERTOUCH_POLY = 0xA0,
		MESSAGE_CONTROL_CHANGE = 0xB0,
		MESSAGE_PROGRAM_CHANGE = 0xC0,
		MESSAGE_AFTERTOUCH_CHANNEL = 0xD0,
		MESSAGE_PITCHWHEEL = 0xE0,
		MESSAGE_SYSEX = 0xF0,
		MESSAGE_SYSEX_END = 0xF7,
		MESSAGE_ACTIVE_SENSE = 0xFE,
		MESSAGE_RESET = 0xFF
	};
	
	enum {							// Partial listing of MIDI controllers
		CONTROL_BANK_SELECT = 0,
		CONTROL_MODULATION_WHEEL = 1,
		CONTROL_VOLUME = 7,
		CONTROL_PATCH_CHANGER = 14,	// Piano bar patch-changing interface (deprecate this?)
		CONTROL_AUX_PEDAL = 15,		// Use this as an auxiliary pedal
		CONTROL_MRP_BASE = 16,		// Base of a range of controllers used by MRP signal routing hardware
		CONTROL_BANK_SELECT_LSB = 32,
		CONTROL_MODULATION_WHEEL_LSB = 33,
		CONTROL_VOLUME_LSB = 39,
		CONTROL_DAMPER_PEDAL = 64,
		CONTROL_SOSTENUTO_PEDAL = 66,
		CONTROL_SOFT_PEDAL = 67,
		CONTROL_ALL_SOUND_OFF = 120,
		CONTROL_ALL_CONTROLLERS_OFF = 121,
		CONTROL_LOCAL_KEYBOARD = 122,
		CONTROL_ALL_NOTES_OFF = 123,
		CONTROL_OMNI_OFF = 124,
		CONTROL_OMNI_ON = 125,
		CONTROL_MONO_OPERATION = 126,
		CONTROL_POLY_OPERATION = 127
	};
	
	enum {							// Piano damper states (excluding effect of damper pedal)
		DAMPER_DOWN = 0,			// Not lifted
		DAMPER_KEY = 1,				// Lifted by key
		DAMPER_SOSTENUTO = 2		// Held by sostenuto pedal
		// Key and Sostenuto: 3
	};
	
	// ************ Constructor *******************
	
	MidiController(AudioRender *render);
	
	// ************ XML Input/Output *******************
	
	int loadPatchTable(string& filename);			// Load patch information from a file.  Returns 0 on success.
	
	// ************ Calibration *******************
	int loadCalibrationTable(string& filename);		// Load calibration information from file.  Returns 0 on success.
	int saveCalibrationTable(string& filename);		// Save current values to a file.  Returns 0 on success.
	void clearCalibration();
	
	// ************ MIDI Input ********************
	
    void setPNOscanController(PNOscanController *controller)    {   PNOcontroller_ = controller;  use_PA_ = true;   }
	void setNumInputDevices(int devices, bool clearState);
	void allControllersOff(int midiChannel);		// Reset all controller values (-1 = all channels)
	void clearInputState();			// Clear stored controller and patch values, remove all listeners, all notes off
    
	// Call these only from the console thread, never from the MIDI thread, since they lock the same mutex
	void consoleProgramChange(int program);				// Program change methods
	int consoleProgramIncrement();
	int consoleProgramDecrement();
	void consoleAllNotesOff(int midiChannel);
	
	unsigned char getControllerValue(int channel, int controller);
	unsigned char getPatchValue(int channel);
	unsigned int getPitchWheelValue(int channel);
	
	void setNoteDisabledChannels(vector<int>& channels);	// Disable these channels from triggering notes
	void setDisplaceOldNotes(bool d) { displaceOldNotes_ = d; }	// Whether to remove old notes when we run out of channels
	
	// The static callback below is needed to interface with RtMidi; it passes control off to the instance-specific function
	void rtMidiCallback(double deltaTime, vector<unsigned char> *message, int inputNumber);	// Instance-specific callback
	static void rtMidiStaticCallback(double deltaTime, vector<unsigned char> *message, void *userData)
	{
		midiCallbackStruct *s = (midiCallbackStruct *)userData;
		(s->controller)->rtMidiCallback(deltaTime, message, s->inputNumber);
	}
	
	// ************ MIDI Output *******************
	
	// Call this before any other MIDI output is used
	void setMidiOutputDevice(RtMidiOut *device) { midiOut_ = device; }
	
	// Specific function to communicate with MRP signal-routing hardware, telling it to connect a particular input to
	// a given piano string
    
	void mrpSetMidiChannel(int channel) { mrpChannel_ = channel & 0xFF; }	// Default value is 0
    void mrpSetBaseAndDirection(int firstString, bool directionDown, int offValue);   // For USB controller
	void mrpSendRoutingMessage(int inputNumber, int stringNumber);
	void mrpClearRoutingTable();
	
	// ************ OSC Methods *******************
	
	// Call this before any OSC-using notes are used (overrides OscHandler implementation)
	void setOscController(OscController *c);
	
	// OSC handler routine
	bool oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data);
    bool oscHandleGlobalParameters(const char *path, const char *types, int numValues, lo_arg **values, void *data);
	bool oscSendPatchValue(); // Send the current patch value by OSC
	
	// ********** Note Callback Methods ***********
	
	// These methods are called by Note objects to register themselves for updates to control change,
	// aftertouch, and pitch wheel MIDI input data.
	
	void addEventListener(Note *note, bool control, bool aftertouch, bool pitchWheel, bool otherNotes);
	void removeEventListener(Note *note);
	
	void noteEnded(Note *note, unsigned int key);					// Called when a Note finishes to request removal from the map
	
	// ********** Utility Methods *****************
    
	float a4Tuning() { return a4Tuning_; }
	void setA4Tuning(float tuning);									// Retune the default frequencies
	
	float midiNoteToFrequency(int note) {							// Convert a MIDI note number into a frequency
		if(note < 0 || note > 127) return 0.0;
		return noteFrequencies_[note];
	}
	bool pianoDamperLifted(int note);								// Tell us whether a given piano damper is up
	int pianoDamperState(int note) {								// Return the specific state of a given damper
		if(note < 0 || note > 127) return DAMPER_DOWN;
		return pianoDamperStates_[note];
	}
	static vector<int> parseRangeString(const string& inString);
	
	// *********** Cleanup Thread *****************
	
	// The cleanup thread regularly calls these functions to check whether any notes have finished.  The static function
	// passes control to the instance-specific function, which polls the available notes to see whether they've finished.
	// If so, it aborts the given note.  This allows notes to finish on timers rather than solely on events.
	
	static void *cleanupLoop(void *data);
    
	// ************* Destructor *******************
	
	~MidiController();
	
private:
	// *********** Private Methods ****************
	void noteOn(double deltaTime, vector<unsigned char> *message, int inputNumber);
	void noteOff(double deltaTime, vector<unsigned char> *message, int inputNumber);
	void damperPedalChange(unsigned char value);		// Called when the damper pedal changes on any channel
	void sostenutoPedalChange(unsigned char value);		// Called when the sostenuto pedal changes on any piano
	
	void allNotesOff(int midiChannel);				// Turn off all sounding notes for channel (-1 = all channels)
	
	void checkForProgramUpdate(int midiChannel, int midiNote);
	
	// ************** Variables *******************
	
    PNOscanController *PNOcontroller_;          // Pointer to the object that handles QRS PNOscan-specific methods
	AudioRender *render_;                       // Pointer to the object that handles all the audio rendering
	RtMidiOut *midiOut_;
	PitchTrackController *pitchTrackController_;
	int mrpChannel_;				// Which MIDI channel the MRP controller listens on
    int mrpFirstString_;            // MIDI note of the first amplifier
    bool mrpDirectionDown_;         // Whether the amplifiers are in ascending or descending order
	int mrpOffValue_;               // Value that we send to turn off a given channel
    bool use_PA_;                               // Use polyphonic aftertouch as key position
	
	float noteFrequencies_[128];				// Frequency for each MIDI note
	bool displaceOldNotes_;						// Whether, when we run out of channels, the newest or oldest notes take priority
	
	unsigned char inputControllers_[16][128];	// 16 channels x 128 controllers
	//unsigned char inputPatches_[16];			// 16 channels x 1 patch each		--- currently use a global program change system instead
	unsigned int inputPitchWheels_[16];			// 16 channels x 1 value (14 bits)
	unsigned int currentProgram_;				// Global program # changed by MIDI program change message on any channel
	bool canTriggerNoteOnChannel_[16];			// Whether this channel allows MIDI note triggering
	
	unsigned char stringNoteMaps_[128];			// Which string each note  is played on
	int pianoDamperStates_[128];				// Keep track of the damper state for each piano string
    // This **does not** include the effect of the damper pedal
    // We'll probably only use 88 of these but this keeps it synced to MIDI note number
	
	map<unsigned int, Note*> currentNotes_;		// Holds the currently sounding notes, for MIDI note-off purposes
    unsigned int monoVoiceNotes_[16];           // Which note is currently sounding in a defined monophonic voice
	set<Note*> aftertouchListeners_;			// Notes that want to be updated on aftertouch data
	set<Note*> pitchWheelListeners_;			// Notes that want to be updated on pitch wheel changes
	set<Note*> controlListeners_;				// Notes that want to be updated on control changes
	set<Note*> noteListeners_;					// Notes that want to be updated on other note on/off events
	
	pthread_t cleanupThread_;					// Thread identifier that runs the cleanup loop
	pthread_mutex_t eventMutex_;				// This ensures that different MIDI streams and cleanup events happen atomically
    // It is a fairly course-grained control (the whole of a MIDI action takes place
    // within), but it ensures our data doesn't get corrupted by competing events.
	bool cleanupShouldTerminate_;				// Set this to true on exit to let the cleanup thread end
	
	float a4Tuning_;							// Frequency of A4 (nominally 440Hz, but adjustable)
	
	// ************** Calibration *******************
	
	float phaseOffsets_[128];			// Phase offset (0-1) for the fundamental frequency of each note
	float amplitudeOffsets_[128];		// Amplitude adjustment (relative to 1.0) for each note
	string lastCalibrationFile_;		// Filename of the last loaded or saved calibration file
	
	// TODO: Vector-valued phase offsets for harmonics
	
	// *********** Patch Table Info **************
	
	map<string, Note*> patches_;				// Holds a collection of prototype notes indexed by name
	//ProgramInfo programs_[16][128];			// Information about each MIDI program on a per-key basis
	map<unsigned int, ProgramInfo> programs_;
	map<unsigned int, unsigned int> programTriggeredChanges_;	// Holds info on MIDI events causing a program change
	
	// ****** Global Function Controllers *********
	
	int controlMasterVolume_;					// Master volume control ID
	int controlPitchTrackInputMute_, controlPitchTrackInputMuteThresh_;	// Control ID to mute the input, when below threshold
	bool lastPitchTrackInputMute_;
};

#endif // MIDICONTROLLER_H