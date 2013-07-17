/*
 *  osccontroller.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 11/14/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "osccontroller.h"
#include "midicontroller.h"
#include "realtimenote.h"
//#include "audiorender.h"

#pragma mark OscHandler

OscHandler::~OscHandler()
{
	if(oscController_ != NULL)	// Remove (individually) each listener
	{
		set<string>::iterator it;
		
		for(it = oscListenerPaths_.begin(); it != oscListenerPaths_.end(); ++it)
		{
			string pathToRemove = *it;
			oscController_->removeListener(pathToRemove, this);
		}
	}
}

#pragma mark -- Private Methods

// Call this internal method to add a listener to the OSC controller.  Returns true on success.

bool OscHandler::addOscListener(string& path)
{
	if(oscController_ == NULL)
		return false;
	if(oscListenerPaths_.count(path) > 0)
		return false;
	oscListenerPaths_.insert(path);
	oscController_->addListener(path, this);
	return true;
}

bool OscHandler::removeOscListener(string& path)
{
	if(oscController_ == NULL)
		return false;
	if(oscListenerPaths_.count(path) == 0)
		return false;
	oscController_->removeListener(path, this);
	oscListenerPaths_.erase(path);
	return true;
}

#pragma mark OscController

// OscController::handler()
// The main handler method for incoming OSC messages.  From here, we farm out the processing depending
// on the path.  In general all our paths should start with /mrp.  Return 0 if the message has been
// adequately handled, 1 otherwise (so the server can look for other functions to pass it to).

int OscController::handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *data)
{
	bool matched = false;
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "Received OSC message " << path << " [" << types << "]\n";
#endif
	
	if(useOscMidi_)	// OSC MIDI emulation.  Also the most time-sensitive & frequent message so do this first.
	{
		if(!strcmp(path, "/mrp/midi") && argc >= 1)
		{
			if(types[0] == 'm')
				return handleMidi(argv[0]->m[1], argv[0]->m[2], argv[0]->m[3]);
			if(argc >= 3)
				if(types[0] == 'i' && types[1] == 'i' && types[2] == 'i')
					return handleMidi((unsigned char)argv[0]->i, (unsigned char)argv[1]->i, (unsigned char)argv[2]->i);
		}
		if(!strcmp(path, "/mrp/quality/brightness") && argc >= 3)
		{
			if(types[0] == 'i' && types[1] == 'i' && types[2] == 'f')
				return handleRtBrightness(argv);
		}
		if(!strcmp(path, "/mrp/quality/intensity") && argc >= 3)
		{
			if(types[0] == 'i' && types[1] == 'i' && types[2] == 'f')
				return handleRtIntensity(argv);
		}
		if(!strcmp(path, "/mrp/quality/pitch") && argc >= 3)
		{
			if(types[0] == 'i' && types[1] == 'i' && types[2] == 'f')
				return handleRtPitch(argv);
		}
        if(!strcmp(path, "/mrp/quality/pitch/vibrato") && argc >= 3)
		{
			if(types[0] == 'i' && types[1] == 'i' && types[2] == 'f')
				return handleRtPitchVibrato(argv);
		}
		if(!strcmp(path, "/mrp/quality/harmonic") && argc >= 3)
		{
			if(types[0] == 'i' && types[1] == 'i' && types[2] == 'f')
				return handleRtHarmonic(argv);
		}
        if(!strcmp(path, "/mrp/quality/harmonics/raw") && argc >= 3)
		{
			if(types[0] == 'i' && types[1] == 'i' && types[2] == 'f')
				return handleRtHarmonicsRaw(argc, types, argv);
		}
        if(!strcmp(path, "/mrp/volume") && argc >= 1)
		{
			if(types[0] == 'f')
            {
                cout << "setting volume to " << argv[0]->f << endl;
                midiController_->render_->setGlobalAmplitude(argv[0]->f);
				return 0;
            }
		}
        if(!strcmp(path, "/mrp/global/harmonics") && argc == 2)
		{
			if(types[0] == 'i' || types[1] == 'f')
				midiController_->oscHandleGlobalParameters(path, types, 2, argv, data);
		}
        if(!strcmp(path, "/mrp/allnotesoff"))
		{
            allNotesOff();
		}
	}

	string pathString(path);	
	
	if(useThru_)
	{
		// Rebroadcast any matching messages
		
		if(!pathString.compare(0, thruPrefix_.length(), thruPrefix_))
			lo_send_message(thruAddress_, path, msg);
	}
	
	// Check if the incoming message matches the global prefix for this program.  If not, discard it.
	if(pathString.compare(0, globalPrefix_.length(), globalPrefix_))
	{
		cout << "OSC message '" << path << "' received\n";
		return 1;
	}
	
	// Lock the mutex so the list of listeners doesn't change midway through
	pthread_mutex_lock(&oscListenerMutex_);
	
	// Now remove the global prefix and compare the rest of the message to the registered handlers.
	multimap<string, OscHandler*>::iterator it;
	pair<multimap<string, OscHandler*>::iterator,multimap<string, OscHandler*>::iterator> ret;
	string truncatedPath = pathString.substr(globalPrefix_.length(), 
											 pathString.length() - globalPrefix_.length());
	ret = noteListeners_.equal_range(truncatedPath);
	
	it = ret.first;
	while(it != ret.second)
	{
		OscHandler *object = (*it++).second;
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Matched OSC path '" << path << "' to handler " << object << endl;
#endif
		object->oscHandlerMethod(truncatedPath.c_str(), types, argc, argv, data);
		matched = true;
	}
	
	pthread_mutex_unlock(&oscListenerMutex_);
	
	if(matched)		// This message has been handled
		return 0;
	
	printf("Unhandled OSC path: <%s>\n", path);
/*#ifdef DEBUG_MESSAGES
	for (i=0; i<argc; i++) {
		printf("arg %d '%c' ", i, types[i]);
		lo_arg_pp((lo_type)types[i], argv[i]);
		printf("\n");
	}
#endif*/

    return 1;
}

// Adds a specific object listening for a specific OSC message.  The object will be
// added to the internal map from strings to objects.  All messages are preceded by
// a global prefix (typically "/mrp").  Returns true on success.

bool OscController::addListener(string& path, OscHandler *object)
{
	if(object == NULL)
		return false;
	
	pthread_mutex_lock(&oscListenerMutex_);
	noteListeners_.insert(pair<string, OscHandler*>(path, object));
	pthread_mutex_unlock(&oscListenerMutex_);
	
#ifdef DEBUG_MESSAGES
	cout << "Added OSC listener to path '" << path << "'\n";
#endif
	
	return true;
}

// Removes a specific object from listening to a specific OSC message. If path is NULL,
// removes all paths for the specified object.  Returns true if at least one path was
// removed.

bool OscController::removeListener(string& path, OscHandler *object)
{
	if(object == NULL)
		return false;

	bool removedAny = false;

	pthread_mutex_lock(&oscListenerMutex_);	// Lock the mutex so no incoming messages happen in the middle
	
	multimap<string, OscHandler*>::iterator it;
	pair<multimap<string, OscHandler*>::iterator,multimap<string, OscHandler*>::iterator> ret;
	
	// Every time we remove an element from the multimap, the iterator is potentially corrupted.  Realistically
	// there should never be more than one entry with the same object and same path (we check this on insertion).
	
	ret = noteListeners_.equal_range(path);
	
	it = ret.first;
	while(it != ret.second)
	{
		if(it->second == object)
		{
			noteListeners_.erase(it->first);
			removedAny = true;
			break;
		}
		else
			++it;
	}
	
	pthread_mutex_unlock(&oscListenerMutex_);
	
#ifdef DEBUG_MESSAGES	
	if(removedAny)
		cout << "Removed OSC listener from path '" << path << "'\n";	
	else
		cout << "Removal failed to find OSC listener on path '" << path << "'\n";
#endif
	
	return removedAny;
}

// Send an OSC message to the specified path, which will be preprended by the application's
// global path.  Returns true if the message was sent, false if an error occurred.

bool OscController::sendMessage(const char *path, const char *types, ...)
{
	if(path == NULL || types == NULL || transmitAddress_ == NULL)
		return false;
	
	va_list ap;
	bool ret = false;
	
	char *totalPath = (char *)malloc((strlen(path) + globalPrefix_.length())*sizeof(char));
	strcpy(totalPath, globalPrefix_.c_str());
	strcat(totalPath, path);
	
	lo_message msg = lo_message_new();
	va_start(ap, types);
	
	ret = (lo_message_add_varargs(msg, types, ap) == 0);	// Check for success (return of 0)
	if(!ret)
		cerr << "Error in OscController::sendMessage -- lo_message_add_varargs failed.\n";
	else	
		ret = (lo_send_message(transmitAddress_, totalPath, msg) == 0);
	
	va_end(ap);
	lo_message_free(msg);
	free(totalPath);
	return ret;
}

#pragma mark -- Private Methods

// Handle a MIDI message encapsulated by OSC.  This will be a standard 3-byte message, and should be
// passed to the MIDI controller as if it originated from a real MIDI device.
// Returns 0 on success (packet handled).

int OscController::handleMidi(unsigned char byte1, unsigned char byte2, unsigned char byte3)
{
	vector<unsigned char> midiMsg;
	
	if(midiController_ == NULL)
		return 1;
	
	midiMsg.push_back(byte1);
	midiMsg.push_back(byte2);
	midiMsg.push_back(byte3);

	// FIXME: deltaTime
	midiController_->rtMidiCallback(0.0, &midiMsg, OSC_MIDI_CONTROLLER_NUM);
	
	return 0;
}

// These functions handle an OSC message updating one of the qualities of a currently playing note.
// The note number and MIDI channel are specified, pointing to a currently playing note.  If no such
// note exists, nothing happens.

int OscController::handleRtIntensity(lo_arg **arg)
{
	int midiChannel = arg[0]->i;
	int midiNote = arg[1]->i;
	float intensity = arg[2]->f;
	
	unsigned int lookupIndex = (midiChannel << 8) + midiNote;
	
	if(midiController_->currentNotes_.count(lookupIndex) == 0)
		return 1;
	Note *note = midiController_->currentNotes_[lookupIndex];
	if(typeid(*note) != typeid(RealTimeMidiNote))
		return 1;
	
	cout << "Updating intensity for note " << lookupIndex << " to " << intensity << endl;
	
	((RealTimeMidiNote *)note)->setAbsoluteIntensityBase(intensity);
	((RealTimeMidiNote *)note)->updateSynthParameters();
	
	return 0;
}

int OscController::handleRtBrightness(lo_arg **arg)
{
	int midiChannel = arg[0]->i;
	int midiNote = arg[1]->i;
	float brightness = arg[2]->f;
	
	unsigned int lookupIndex = (midiChannel << 8) + midiNote;
    
	if(midiController_->currentNotes_.count(lookupIndex) == 0)
		return 1;
	Note *note = midiController_->currentNotes_[lookupIndex];
	if(typeid(*note) != typeid(RealTimeMidiNote))
		return 1;
	
	cout << "Updating brightness for note " << lookupIndex << " to " << brightness << endl;
	
	((RealTimeMidiNote *)note)->setAbsoluteBrightness(brightness);
	((RealTimeMidiNote *)note)->updateSynthParameters();
	
	return 0;
}

int OscController::handleRtPitch(lo_arg **arg)
{
	int midiChannel = arg[0]->i;
	int midiNote = arg[1]->i;
	float pitch = arg[2]->f;
	
	unsigned int lookupIndex = (midiChannel << 8) + midiNote;
	if(midiController_->currentNotes_.count(lookupIndex) == 0)
		return 1;
	Note *note = midiController_->currentNotes_[lookupIndex];
	if(typeid(*note) != typeid(RealTimeMidiNote))
		return 1;
	
	cout << "Updating pitch for note " << lookupIndex << " to " << pitch << endl;
	
	((RealTimeMidiNote *)note)->setAbsolutePitchBase(pitch);
	((RealTimeMidiNote *)note)->updateSynthParameters();
	
	return 0;
}

int OscController::handleRtPitchVibrato(lo_arg **arg)
{
	int midiChannel = arg[0]->i;
	int midiNote = arg[1]->i;
	float pitch = arg[2]->f;
	
	unsigned int lookupIndex = (midiChannel << 8) + midiNote;
    
	if(midiController_->currentNotes_.count(lookupIndex) == 0)
		return 1;
	Note *note = midiController_->currentNotes_[lookupIndex];
	if(typeid(*note) != typeid(RealTimeMidiNote))
		return 1;
	
	cout << "Updating pitch vibrato for note " << lookupIndex << " to " << pitch << endl;
	
	((RealTimeMidiNote *)note)->setAbsolutePitchVibrato(pitch);
	((RealTimeMidiNote *)note)->updateSynthParameters();
	
	return 0;
}

int OscController::handleRtHarmonic(lo_arg **arg)
{
	int midiChannel = arg[0]->i;
	int midiNote = arg[1]->i;
	float harmonic = arg[2]->f;
	
	unsigned int lookupIndex = (midiChannel << 8) + midiNote;
	if(midiController_->currentNotes_.count(lookupIndex) == 0)
		return 1;
	Note *note = midiController_->currentNotes_[lookupIndex];
	if(typeid(*note) != typeid(RealTimeMidiNote))
		return 1;
	
	cout << "Updating harmonic for note " << lookupIndex << " to " << harmonic << endl;
	
	((RealTimeMidiNote *)note)->setAbsoluteHarmonicBase(harmonic);
	((RealTimeMidiNote *)note)->updateSynthParameters();
	
	return 0;
}

int OscController::handleRtHarmonicsRaw(int argc, const char *types, lo_arg **arg)
{
	int midiChannel = arg[0]->i;
	int midiNote = arg[1]->i;
    
	unsigned int lookupIndex = (midiChannel << 8) + midiNote;
    
	if(midiController_->currentNotes_.count(lookupIndex) == 0)
		return 1;
	Note *note = midiController_->currentNotes_[lookupIndex];
	if(typeid(*note) != typeid(RealTimeMidiNote))
		return 1;
    
    // Raw harmonics are expressed as a vector of values. Find as many
    // values as exist floats in the message
    vector<double> harmonicValues;
    int i = 2;
    
    while(i < argc) {
        if(types[i] != 'f')
            break;
        harmonicValues.push_back(arg[i++]->f);
    }
    
	
	cout << "Updating raw harmonics for note " << lookupIndex << " (" << harmonicValues.size() << " values)" << endl;
	
	((RealTimeMidiNote *)note)->setRawHarmonicValues(harmonicValues);
	
	return 0;
}

// Turn off all notes by way of the MIDI controller
void OscController::allNotesOff()
{
    if(midiController_ != 0)
        midiController_->allNotesOff(-1);
}