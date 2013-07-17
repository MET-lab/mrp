/*
 *  osccontroller.h
 *  mrp
 *
 *  Created by Andrew McPherson on 11/14/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#ifndef OSC_CONTROLLER_H
#define OSC_CONTROLLER_H

#include <iostream>
#include <set>
#include <map>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "lo/lo.h"
#include "config.h"
using namespace std;

class OscController;
class MidiController;
//class AudioRender;

// This is an abstract base class implementing a single function oscHandlerMethod().  Objects that
// want to register to receive OSC messages should inherit from OscHandler.

class OscHandler
{
public:
	OscHandler() : oscController_(NULL) {}
	
	// The OSC controller will call this method when it gets a matching message that's been registered
	virtual bool oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data) = 0;
	void setOscController(OscController *c) { oscController_ = c; }

	~OscHandler();	// In the destructor, remove all OSC listeners
protected:
	bool addOscListener(string& path);
	bool removeOscListener(string& path);
	
	OscController *oscController_;
	set<string> oscListenerPaths_;
};

class OscController 
{
public:
	OscController(lo_server_thread thread, lo_address transmitAddr, const char *prefix) : midiController_(NULL), useOscMidi_(false) {
		oscServerThread_ = thread;
		transmitAddress_ = transmitAddr;
		globalPrefix_.assign(prefix);
		useThru_ = false;
		pthread_mutex_init(&oscListenerMutex_, NULL);
		lo_server_thread_add_method(thread, NULL, NULL, OscController::staticHandler, (void *)this);
	}
	
	void setMidiController(MidiController *controller) { midiController_ = controller; }
	void setUseOscMidi(bool use) { useOscMidi_ = use; }
	void setThruAddress(lo_address thruAddr, const char *prefix) {
		thruAddress_ = thruAddr;
		thruPrefix_.assign(prefix);
		useThru_ = true;
	}
	
	bool addListener(string& path, OscHandler *object);		// Add a listener object for a specific path
	bool removeListener(string& path, OscHandler *object);	// Remove a listener object
	
	// staticHandler() is called by liblo with new OSC messages.  Its only function is to pass control
	// to the object-specific handler method, which has access to all internal variables.
	
	int handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *data);
	static int staticHandler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *userData) {
		return ((OscController *)userData)->handler(path, types, argv, argc, msg, userData);
	}	
	
	// This method allows classes to transmit their own OSC messages.  The variables in "..." should conform to the types
	// specified, and should terminate with LO_ARGS_END.
	
	bool sendMessage(const char *path, const char *types, ...);
    
    // All notes off message, relayed to MIDI
    void allNotesOff();
	
	~OscController() {
		lo_server_thread_del_method(oscServerThread_, NULL, NULL);
		pthread_mutex_destroy(&oscListenerMutex_);
	}
	
private:	
	// MIDI emulation
	int handleMidi(unsigned char byte1, unsigned char byte2, unsigned char byte3);	// Handle OSC-encapsulated MIDI
	int handleRtIntensity(lo_arg **arg);
	int handleRtBrightness(lo_arg **arg);
	int handleRtPitch(lo_arg **arg);
    int handleRtPitchVibrato(lo_arg **arg);
	int handleRtHarmonic(lo_arg **arg);
    int handleRtHarmonicsRaw(int argc, const char *types, lo_arg **arg);
	
	// References to other important objects
	MidiController *midiController_;		// Reference to the main MIDI controller
	lo_server_thread oscServerThread_;		// Thread that handles received OSC messages
	lo_address transmitAddress_;			// Address to which outgoing messages should be sent
	
	// OSC thru
	bool useThru_;							// Whether or not we retransmit any messages
	lo_address thruAddress_;				// Address to which we retransmit
	string thruPrefix_;						// Prefix that must be matched to be retransmitted
	
	// State variables
	bool useOscMidi_;						// Whether we use OSC MIDI emulation
	string globalPrefix_;					// Prefix for all OSC paths
	pthread_mutex_t oscListenerMutex_;		// This mutex protects the OSC listener table from being modified mid-message
	
	multimap<string, OscHandler*> noteListeners_;	// Map from OSC path name to handler (possibly multiple handlers per object)
};

#endif // OSC_CONTROLLER_H