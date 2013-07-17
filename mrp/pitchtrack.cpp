/*
 *  pitchtrack.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 11/14/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "pitchtrack.h"
#include "wavetables.h"
#include <cstring>
#include <cmath>

#define TWELVE_OVER_LN2	(17.312340490667561)	// 12/log(2)
#define PTRK_PROGRAM_ID(prog, note) (unsigned int)((prog << 8) + note)

extern WaveTable waveTable;

#pragma mark PitchTrackController

PitchTrackController::PitchTrackController(MidiController *midiController)
{
	int i;
	
	midiController_ = midiController;
	midiController_->pitchTrackController_ = this;	// Give the MidiController our reference

	// Set default values
	bzero(frequencyBuffer_, PITCHTRACK_BUFFER_SIZE*sizeof(float));
	bzero(amplitudeBuffer_, PITCHTRACK_BUFFER_SIZE*sizeof(float));
	bzero(fractionalMidiNoteBuffer_, PITCHTRACK_BUFFER_SIZE*sizeof(float));
	bzero(pitchSampleCount_, 128*sizeof(int));
	for(i = 0; i < 128; i++)
	{
		soundingNotePriorities_[i] = -1;	// < 0 means not sounding
		soundingNoteStartTimes_[i] = -1.0;
		notesTriggered_[i] = false;
	}
	
	frequencyBufferIndex_ = amplitudeBufferIndex_ = fractionalMidiNoteBufferIndex_ = 0;
	triggerTotalSamples_ = 4;
	triggerPositiveSamples_ = 3;
	pitchToleranceSemitones_ = 0.5;	// TODO: Implement an XML structure for these
	amplitudeThreshold_ = 0.0;
	allowOctaveErrors_ = false;
	inputMuted_ = false;
	
	if(pthread_mutex_init(&listenerMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize parameterMutex in PitchTrackSynth\n";
		// Throw exception?
	}		
	
#ifdef DEBUG_OSC
	oscCounter_ = 0;
#endif
}

// Here we parse the patch table to get general triggering settings

void PitchTrackController::parseGlobalSettings(TiXmlElement *baseElement)
{
	// The element we look for is called <PitchTrackSettings>.  Inside we have
	// several parameters for how notes are triggered.
	
	if(baseElement == NULL)
		return;
	
	TiXmlElement *settingsElement = baseElement->FirstChildElement("PitchTrackSettings");
	
	if(settingsElement != NULL)
	{
		// Look for the following tags
		// <TriggerTotalSamples>
		// <TriggerPositiveSamples> -- how many positive samples out of the total trigger a note
		// <PitchTolerance> -- within what fraction of a semitone a frequency matches a given note
	    // <AmplitudeThreshold> -- how strong the amplitude must be before a match is triggered
		
		TiXmlHandle settingsHandle(settingsElement);
		TiXmlText *text = settingsHandle.FirstChildElement("TriggerTotalSamples").FirstChild().ToText();
		if(text != NULL)
		{
			stringstream s(text->ValueStr());
			s >> triggerTotalSamples_;
			
			//cout << "Trigger total samples = " << triggerTotalSamples_ << endl;
		}
		text = settingsHandle.FirstChildElement("TriggerPositiveSamples").FirstChild().ToText();
		if(text != NULL)
		{
			stringstream s(text->ValueStr());
			s >> triggerPositiveSamples_;
			
			//cout << "Trigger positive samples = " << triggerPositiveSamples_ << endl;
		}
		text = settingsHandle.FirstChildElement("PitchTolerance").FirstChild().ToText();
		if(text != NULL)
		{
			stringstream s(text->ValueStr());
			s >> pitchToleranceSemitones_;
			
			//cout << "Pitch tolerance = " << pitchToleranceSemitones_ << endl;
		}		
		text = settingsHandle.FirstChildElement("AmplitudeThreshold").FirstChild().ToText();
		if(text != NULL)
		{
			stringstream s(text->ValueStr());
			s >> amplitudeThreshold_;
			
			//cout << "Amplitude threshold = " << amplitudeThreshold_ << endl;
		}				
	}
}

// This method is called by MidiController when it encounters a PitchTrack tag.  We load our
// own custom data from the given element (representing <PitchTrack>)

void PitchTrackController::parsePatchTable(TiXmlElement *pitchTrackElement, int programId)
{
	TiXmlHandle pitchTrackHandle(pitchTrackElement);
	TiXmlText *text;
	string patchName;
	PitchTrackProgramInfo info;
	bool foundNote = false;
	vector<int> noteNumbers; 

	// Before anything, see if this program disables any old notes
	
	text = pitchTrackHandle.FirstChildElement("NotesOff").FirstChild().ToText();
	if(text != NULL)
	{
		if(text->ValueStr() == (const string)"all")
		{
			vector<int> notesOff;
			int i;
			
			for(i = 0; i < 128; i++)
				notesOff.push_back(i);
			
			notesToTurnOff_[programId] = notesOff;
		}
		else
		{
			vector<int> notesOff = parseCommaSeparatedValues(text->ValueStr());
		
			notesToTurnOff_[programId] = notesOff;
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Notes off:";
		for(int i = 0; i < notesOff.size(); i++)
			cout << " " << notesOff[i];
		cout << endl;
#endif
		}
	}	
	
	// Load patch for this particular program
	
	TiXmlElement *patchElement = pitchTrackElement->FirstChildElement("Patch");
	
	if(patchElement != NULL)
	{
		TiXmlHandle patchHandle(patchElement);

		// Get Patch name for this channel
		text = patchHandle.FirstChild().ToText();
		if(text != NULL)
		{
			
			patchName = text->ValueStr();
			Note *noteToSave = midiController_->patches_[text->ValueStr()];		// Look up in patch table
			
			if(noteToSave != NULL)
			{
				if(typeid(*noteToSave) != typeid(PitchTrackNote))
				{
					cerr << "parsePatchTable() error: need PitchTrackNote type for program " << programId << endl;
				}
				else
				{
					foundNote = true;
					info.note = (PitchTrackNote *)noteToSave;
				}
			}
		}
	}
	
	if(!foundNote)
	{
#ifdef DEBUG_MESSAGES
		cout << "PitchTrackController::parsePatchTable(): no note for program " << programId << endl;
#endif
		return;
	}
	
	// Fill out other info: onceOnly, priority, coupled notes, 
	info.onceOnly = false;
	info.priority = 0;
	
	text = pitchTrackHandle.FirstChildElement("OnceOnly").FirstChild().ToText();
	if(text != NULL)
	{
		stringstream s(text->ValueStr());
		s >> boolalpha >> info.onceOnly;
		//cout << "Once only: " << info.onceOnly << endl;
	}
	text = pitchTrackHandle.FirstChildElement("Priority").FirstChild().ToText();
	if(text != NULL)
	{
		stringstream s(text->ValueStr());
		s >> info.priority;
		//cout << "Priority: " << info.priority << endl;
	}
	
	text = pitchTrackHandle.FirstChildElement("Coupling").FirstChild().ToText();
	if(text != NULL)
	{
		info.coupledNotes = parseCommaSeparatedValues(text->ValueStr());
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Coupled notes:";
		for(int i = 0; i < info.coupledNotes.size(); i++)
			cout << " " << info.coupledNotes[i];
		cout << endl;
#endif
	}
	
	// Finally, store the structure we've loaded into the program table
	// Get note range (optional for MidiController but required here).
	
	text = pitchTrackHandle.FirstChildElement("Range").FirstChild().ToText();
	if(text != NULL)
		noteNumbers = midiController_->parseRangeString(text->ValueStr());
	
	if(noteNumbers.size() > 0)
	{
		vector<int>::iterator it;
		for(it = noteNumbers.begin(); it != noteNumbers.end(); it++)
		{
			if((*it) < 0 || (*it) > 127)
				continue;
			programs_[PTRK_PROGRAM_ID(programId, *it)] = info;
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "parsePatchTable(): Added patch " << patchName << " to note " << (*it) << endl;
#endif
		}
	}
	else	// Pitch tracker requires a range parameter to prevent total chaos
		cerr << "PitchTrackController::parsePatchTable warning: no range specified, skipping program " << programId << endl;

	// Check if there are any notes to enable immediately, regardless of input
	text = pitchTrackHandle.FirstChildElement("NotesOn").FirstChild().ToText();
	if(text != NULL)
	{
		vector<int> notesOn = parseCommaSeparatedValues(text->ValueStr());
		
		notesToTurnOn_[programId] = notesOn;
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Notes on:";
		for(int i = 0; i < notesOn.size(); i++)
			cout << " " << notesOn[i];
		cout << endl;
#endif
	}	
}

void PitchTrackController::allNotesOff()
{
	// Send note-off message to everything in the currentNotes collection	
	
	map<unsigned int, PitchTrackNote*>::iterator it = pitchTrackCurrentNotes_.begin();
	PitchTrackNote *oldNote;
	
	while(it != pitchTrackCurrentNotes_.end())
	{
		unsigned int key = (*it).first;
#ifdef DEBUG_MESSAGES_EXTRA
		unsigned int channel = ((*it).second)->midiChannel();
		cout << "PTC::allNotesOff(): found note with key " << key << ", channel " << channel << endl;
#endif

		oldNote = (*it++).second;
		midiController_->mrpSendRoutingMessage(oldNote->mrpChannel(), 0);		// Disconnect the signal router	
		oldNote->abort();					// Forcibly stop the note
		if(pitchTrackCurrentNotes_.count(key) > 0)	// If the Note object has not removed itself during abort(), remove it from the map
		{	
			pitchTrackCurrentNotes_.erase(key);
		}
		
		//delete oldNote;		// Shouldn't need....
	}	
	
	midiController_->mrpClearRoutingTable();
}

// This is comparable to the method of the same name in MidiController
void PitchTrackController::noteEnded(PitchTrackNote *note, unsigned int key)
{
	// Note objects call this when they finish releasing.
	// We should remove the indicated note from the map of current notes and return its audio channel to the pool.
	// Finally, delete the Note object.
	
#ifdef DEBUG_MESSAGES
	cout << "PTC: Note with key " << key << " ended.\n";
#endif
	
	midiController_->render_->freeOutputChannel(note->audioChannel());	// Return output channel to the pool
	midiController_->mrpSendRoutingMessage(note->mrpChannel(), 0);		// Disconnect the signal routing for this string
	
	if(pitchTrackCurrentNotes_.count(key) > 0)	// If the Note object has not removed itself during abort(), remove it from the map
	{
		pitchTrackCurrentNotes_.erase(key);
		
		// In this case, the key should be just the note ID; we'll make sure it's in range just in case
		if(key >= 0 && key < 128)
		{
			soundingNotePriorities_[key] = -1;
			soundingNoteStartTimes_[key] = -1.0;
		}
	}
	else
		cerr << "PTC Warning: attempt to remove nonexistent key " << key << endl;
	
	delete note;		// Will this crash??
}

// Reset the info that says whether each note has been triggered.  This will allow once-only notes
// to sound again.  In general we'll want to do this every time MidiController's patch change is updated

void PitchTrackController::programChanged()
{
	int i;
	
	pthread_mutex_lock(&listenerMutex_);
	
	// Disable any notes that are set to turn off
	if(notesToTurnOff_.count(midiController_->currentProgram_) > 0)
	{
		vector<int> notesOff = notesToTurnOff_[midiController_->currentProgram_];
		
		for(i = 0; i < notesOff.size(); i++)
		{
			if(pitchTrackCurrentNotes_.count(notesOff[i]) > 0)
			{
				PitchTrackNote *note = pitchTrackCurrentNotes_[notesOff[i]];
			
				if(note != NULL)
				{
#ifdef DEBUG_MESSAGES
					cout << "Aborting note " << notesOff[i] << endl;
#endif
					note->abort();
				}
			}
		}
	}
	
	pthread_mutex_unlock(&listenerMutex_);
	
	// Reset the note triggers
	for(i = 0; i < 128; i++)
		notesTriggered_[i] = false;
	
	// Turn on any notes that are supposed to be enabled immediately in this program
	if(notesToTurnOn_.count(midiController_->currentProgram_) > 0)
	{
		vector<int> notesOn = notesToTurnOn_[midiController_->currentProgram_];
		
		for(i = 0; i < notesOn.size(); i++)
		{
			int noteId = notesOn[i];
			
			// Trigger this note now
			int program = PTRK_PROGRAM_ID(midiController_->currentProgram_, noteId);
			if(programs_.count(program) > 0 && soundingNotePriorities_[noteId] < 0 && !notesTriggered_[noteId])
			{
				triggerNote(programs_[program].note, noteId, noteId, programs_[program].priority);
				
				// See if there are any coupled notes to trigger.  Couplings are expressed
				// in semitone offsets (i.e. -1 = one semitone lower, 12 = one octave higher)
				for(int j = 0; j < programs_[program].coupledNotes.size(); j++)
				{
					triggerNote(programs_[program].note, noteId + programs_[program].coupledNotes[j],
								noteId, programs_[program].priority);
				}
				// If this was a once-only trigger, set the flag saying it was triggered so it doesn't
				// happen again (unless this patch is reloaded).
				if(programs_[program].onceOnly)
					notesTriggered_[noteId] = true;		
			}
		}		
	}	
}

// Handle input samples of the format {frequency, amplitude}.  Frequency is in Hz, amplitude on a 0-1 scale.
// Return true on success.

bool PitchTrackController::handleOscPitch(const char *path, const char *types, int numValues, lo_arg **argv, void *data)
{
	float frequency, amplitude, fractionalMidiNote, oldNote;
	int lowestPitchMatch, highestPitchMatch, i;
	map<unsigned int, PitchTrackNote *>::iterator it;
	
	if(types[0] != LO_FLOAT || types[1] != LO_FLOAT)
	{
		cerr << "PitchTrackController::handlePitchData(): expect type ff, found " << types << "\n";
		return false;			
	}
	
#ifdef DEBUG_OSC
	oscCounter_++;
	if(oscCounter_ % OSC_COUNTER_INTERVAL == 0)		// Print tick marks to indicate OSC is working
	{
		cout << ".";
		cout.flush();
	}
#endif
	
	frequency = argv[0]->f;	// Extract the values from the OSC message
	amplitude = argv[1]->f;
	
	// Convert this frequency to a fractional MIDI note; this normalizes frequency differences across
	// octaves and makes it easier to compare later whether the pitch tests have been met
	
	if(amplitude >= amplitudeThreshold_ && frequency >= 0)
		fractionalMidiNote = 69.0 + TWELVE_OVER_LN2*log(frequency/midiController_->a4Tuning());
	else
		fractionalMidiNote = 0.0;
	
	frequencyBufferIndex_ = (frequencyBufferIndex_ + 1) % PITCHTRACK_BUFFER_SIZE;
	amplitudeBufferIndex_ = (amplitudeBufferIndex_ + 1) % PITCHTRACK_BUFFER_SIZE;
	fractionalMidiNoteBufferIndex_ = (fractionalMidiNoteBufferIndex_ + 1) % PITCHTRACK_BUFFER_SIZE;
	
	frequencyBuffer_[frequencyBufferIndex_] = frequency;	// Store values in the circular buffer
	amplitudeBuffer_[amplitudeBufferIndex_] = amplitude;
	fractionalMidiNoteBuffer_[fractionalMidiNoteBufferIndex_] = fractionalMidiNote;	
	
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "Pitch tracking: frequency " << frequency << " amplitude " << amplitude << " (pitch " << fractionalMidiNote << ")\n";
#endif	
	
	// Find the range of pitches that could conceivably match this note, but first, do this with
	// the note triggerTotalSamples_ ago, to remove its effect from the accumulator
	oldNote = fractionalMidiNoteBuffer_[(fractionalMidiNoteBufferIndex_ + PITCHTRACK_BUFFER_SIZE - triggerTotalSamples_)%PITCHTRACK_BUFFER_SIZE];
	lowestPitchMatch = ceilf(oldNote - pitchToleranceSemitones_);
	highestPitchMatch = floorf(oldNote + pitchToleranceSemitones_);
	
	if(oldNote > 0.0)
	{
		if(lowestPitchMatch < 0)
			lowestPitchMatch = 0;
		if(highestPitchMatch > 127)
			highestPitchMatch = 127;
		
		for(i = lowestPitchMatch; i <= highestPitchMatch; i++)
		{
			if(pitchSampleCount_[i] > 0)
				pitchSampleCount_[i]--;
		}
	}
	
	lowestPitchMatch = ceilf(fractionalMidiNote - pitchToleranceSemitones_);
	highestPitchMatch = floorf(fractionalMidiNote + pitchToleranceSemitones_);
	
	// Decide whether to allocate new notes
	// It will always be the most recent sample that puts us over the threshold for any given note,
	// so we should examine how this note relates to the previous history.  The range of interest is
	// plus or minus the threshold size.
	
	if(fractionalMidiNote > 0.0 && !inputMuted_)
	{
		if(lowestPitchMatch < 0)
			lowestPitchMatch = 0;
		if(highestPitchMatch > 127)
			highestPitchMatch = 127;
		
		for(i = lowestPitchMatch; i <= highestPitchMatch; i++)
		{
			pitchSampleCount_[i]++;
			if(pitchSampleCount_[i] >= triggerPositiveSamples_)
			{
				// Matched this pitch; trigger a new note if there's one in the program table
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Matched note " << i << endl;
#endif
				int program = PTRK_PROGRAM_ID(midiController_->currentProgram_, i);
				if(programs_.count(program) > 0 && soundingNotePriorities_[i] < 0 && !notesTriggered_[i])
				{
					triggerNote(programs_[program].note, i, i, programs_[program].priority);
					
					// See if there are any coupled notes to trigger.  Couplings are expressed
					// in semitone offsets (i.e. -1 = one semitone lower, 12 = one octave higher)
					for(int j = 0; j < programs_[program].coupledNotes.size(); j++)
					{
						triggerNote(programs_[program].note, i + programs_[program].coupledNotes[j],
									i, programs_[program].priority);
					}
					// If this was a once-only trigger, set the flag saying it was triggered so it doesn't
					// happen again (unless this patch is reloaded).
					if(programs_[program].onceOnly)
						notesTriggered_[i] = true;		
				}
			}
		}
	}
	
	pthread_mutex_lock(&listenerMutex_);
	
	// Alert existing notes to changes in pitch track value in case they want to do anything
	it = pitchTrackCurrentNotes_.begin();
	
	while(it != pitchTrackCurrentNotes_.end())
	{
		// If the input is muted, don't suppress the message, because the timing disruption may lead
		// the notes to do funny things.  Instead, send zeroes.
		
		if(inputMuted_)
			(*it++).second->pitchTrackValues(0.0, 0.0);
		else
			(*it++).second->pitchTrackValues(frequency, amplitude);
	}
	
	pthread_mutex_unlock(&listenerMutex_);
	
	return true;	
}

// Handle the muting function via OSC

bool PitchTrackController::handleOscMute(const char *path, const char *types, int numValues, lo_arg **argv, void *data)
{
	if(types[0] == LO_FLOAT) {
		if(argv[0]->f >= 0.5) {
			cout << "PitchTrackController: muting input\n";
			setInputMute(true);
		}
		else {
			cout << "PitchTrackController: unmuting input\n";
			setInputMute(false);
		}
		
		return true;
	}
	else if(types[0] == LO_INT32) {
		if(argv[0]->i != 0) {
			cout << "PitchTrackController: muting input\n";
			setInputMute(true);
		}
		else {
			cout << "PitchTrackController: unmuting input\n";
			setInputMute(false);
		}
		
		return true;
	}
	
	cout << "PitchTrackController::handleOscMute(): unknown type " << types << endl;
	return false;
}

// PitchTrackController::oscHandler()
// Messages should be of type "/mrp/ptrk/..." to get here, and will contain two floats.  The first holds
// frequency, the second holds amplitude.
// Use this information to decide what notes to trigger, and pass along information to any notes that
// want to hear it.

bool PitchTrackController::oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **argv, void *data)
//int PitchTrackController::oscHandler(const char *path, const char *types, lo_arg **argv, int argc, void *data)
{
	if(!strcmp(path, "/ptrk/pitch") && numValues >= 2)
		return handleOscPitch(path, types, numValues, argv, data);
	else if(!strcmp(path, "/ptrk/mute") && numValues >= 1)
		return handleOscMute(path, types, numValues, argv, data);
	
	cerr << "PitchTrackController::oscHandlerMethod(): found unknown path " << path << " with " << numValues << " args\n";
	return false;
}

// Register the OSC controller and register for relevant messages

void PitchTrackController::setOscController(OscController *c)
{
	OscHandler::setOscController(c);
	
	string str1("/ptrk/pitch");
	string str2("/ptrk/mute");
	
	addOscListener(str1);
	addOscListener(str2);
}

// This function is called when a pitch match is found to one of the values in the program table.
// It allocates a new audio channel, freeing old notes if necessary, and starts a new note playing.
// inputMidiNote is used to set the frequency at which the note responds to the pitch tracker.  If
// this is a coupled note, inputMidiNote may be different than midiNoteId
// Returns 0 on success.


int PitchTrackController::triggerNote(PitchTrackNote *noteRef, int midiNoteId, int inputMidiNote, int priority)
{
	PitchTrackNote *newNote;
	int pianoString, i, minPriority = priority;
	pair<int,int> channels;
	vector<int> lowPriorityNotes;
	double currentTime = midiController_->render_->currentTime();
	double earliestTime = currentTime;
	int earliestTimeNote = -1;
	PitchTrackNote *earliestNote;
	
#ifdef DEBUG_MESSAGES
	cout << "Triggering note " << midiNoteId << endl;
#endif
	
	// First, allocate a channel for this note
	channels = midiController_->render_->allocateOutputChannel();
	if(channels.first == -1)									// No channel available, or error occurred
	{
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "No channel available for note " << midiNoteId << ", searching...\n";
#endif
		// Search for lower-priority notes to free
		// If notes of the same priority, turn off by oldest start time
		for(i = 0; i < 128; i++)
		{
			// Valid candidates for turnoff are those that are on but which have equal or lesser priority
			// Compile a vector of all notes with the lowest (same) priority, and from there we'll decide
			// which one started first and thus, which one to stop now.
			
			if(soundingNotePriorities_[i] >= 0 && soundingNotePriorities_[i] <= priority)
			{
				if(soundingNotePriorities_[i] < minPriority)	// Lower priority than what we currently have
				{
					lowPriorityNotes.clear();
					lowPriorityNotes.push_back(i);
					minPriority = soundingNotePriorities_[i];
				}
				else if(soundingNotePriorities_[i] == minPriority)
					lowPriorityNotes.push_back(i);
			}
		}
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Found notes with priority " << minPriority << ":";
		for(i = 0; i < lowPriorityNotes.size(); i++)
			cout << " " << lowPriorityNotes[i];
		cout << endl;
#endif
		
		for(i = 0; i < lowPriorityNotes.size(); i++)
		{
			if(soundingNoteStartTimes_[lowPriorityNotes[i]] < earliestTime)
			{
				earliestTime = soundingNoteStartTimes_[lowPriorityNotes[i]];
				earliestTimeNote = lowPriorityNotes[i];
			}
		}
		if(earliestTimeNote == -1)	// Didn't find any suitable candidates to release to make room
		{
			cerr << "No notes available to free.  Note canceled\n";
			return 1;
		}		
		
#ifdef DEBUG_MESSAGES
		cout << "No channels available; stopping note " << earliestTimeNote << endl;
#endif
		earliestNote = pitchTrackCurrentNotes_[earliestTimeNote];
		if(earliestNote == NULL)
		{
			cerr << "Error: did not find Note object for about-to-be-released note " << earliestTimeNote << endl;
			return 1;
		}
		
		// Tell the early-note candidate to abort so we can have the channel back
		earliestNote->abort();
		
		// Now try to reallocate the audio channel
		channels = midiController_->render_->allocateOutputChannel();
		if(channels.first == -1)	// Still couldn't find a channel!
		{
			cerr << "No output channel available for note " << midiNoteId << ". Note canceled\n";
			return 1;
		}
	}
	
	// Send a MIDI output message (if we're using MIDI out) to the router hardware,
	// sending this note to the correct string
	
	pianoString = midiController_->stringNoteMaps_[midiNoteId];
	midiController_->mrpSendRoutingMessage(channels.second, pianoString);

#ifdef DEBUG_MESSAGES_EXTRA
	cout << "String " << pianoString << ", audio channel " << channels.first << endl;
#endif	
	// Create a new note based on the prototype in the program table
	newNote = noteRef->createNote(channels.first, channels.second, midiNoteId, inputMidiNote, pianoString, 
						 midiNoteId, priority,
						 127, /* velocity not used */
						 midiController_->phaseOffsets_[midiNoteId], 
						 midiController_->amplitudeOffsets_[midiNoteId]);
	
	newNote->begin(true);		// Tell this note to begin
	
	// Store this in the set of current notes
	pitchTrackCurrentNotes_[midiNoteId] = newNote;
	
	// Set flag to show this note is active, so it doesn't attempt to retrigger the very next
	// sample if the pitch is the same.
	soundingNotePriorities_[midiNoteId] = priority;	
	soundingNoteStartTimes_[midiNoteId] = currentTime;
	
	return 0;
}


vector<int> PitchTrackController::parseCommaSeparatedValues(const string& inString)
{
	vector<int> out;
	char *str, *strOrig, *ap;

	str = strOrig = strdup(inString.c_str());

	// Parse a string of the format "0, 1, -1, 4" containing a list of int values, and put the values into a vector
	// Strings of the form "1,,,,,,3,4" are acceptable-- empty values are ignored

	while((ap = strsep(&str, ",; \t")) != NULL)
	{
		if((*ap) != '\0')
			out.push_back(strtod(ap, NULL));
	}

	free(strOrig);	// This pointer won't move like str
	return out;
}

#pragma mark PitchTrackNote

PitchTrackNote::PitchTrackNote(MidiController *controller, AudioRender *render, 
							   PitchTrackController *ptController) : MidiNote(controller, render)
{
#ifdef DEBUG_ALLOCATION
	cout << "*** PitchTrackNote\n";	
#endif
	
	pitchTrackController_ = ptController;
	sustainOnDamperPedal_ = sustainOnSostenutoPedal_ = false;		// Don't use these in this note
	
	synths_.clear();
	factories_.clear();
	relativeInputFrequencies_.clear();
	relativeOutputFrequencies_.clear();
}

// Parse an XML data structure to get all the parameters we need.  This extends the MidiNote method to
// pick up some pitch-tracking-specific parameters

int PitchTrackNote::parseXml(TiXmlElement *baseElement)
{
	// Presently, we look for Synth tags within this element.  A note can have one or more synths, each of which
	// has a set of ramping parameters.
	int numSynths = 0;
	TiXmlElement *synthElement = baseElement->FirstChildElement("Synth");
	const string *name = baseElement->Attribute((const string)"name");
	
	if(name != NULL)
		name_ = *name;
	
	synths_.clear();				// Clear out old data
	factories_.clear();
	relativeInputFrequencies_.clear();
	relativeOutputFrequencies_.clear();
	
	while(synthElement != NULL)
	{
		const string *synthClass = synthElement->Attribute((const string)"class");	// Attribute tells us the kind of synth
		
		if(synthClass == NULL)		// If we can't find a class attribute, skip it
		{
			cerr << "PitchTrackNote::parseXml() warning: Synth object with no class\n";
			synthElement = synthElement->NextSiblingElement("Synth");
			continue;
		}
		if(synthClass->compare("PitchTrackSynth") == 0 )
		{
			// This is the only type of synth we respond to (for now) in this class.
			
			// Each synth holds a collection of parameters
			TiXmlElement *paramElement = synthElement->FirstChildElement("Parameter");
			PitchTrackSynth *ptSynth = new PitchTrackSynth(render_->sampleRate());
			paramHolder pHin, pHout;
			timedParameter emptyParam;
			
			pHin.start = pHout.start = 1.0;
			pHin.ramp = pHout.ramp = emptyParam;
			
			while(paramElement != NULL)
			{
				assignPitchTrackSynthParameters(ptSynth, &pHin, &pHout, paramElement);
				paramElement = paramElement->NextSiblingElement("Parameter");
			}
			
			synths_.push_back(ptSynth);			
			relativeInputFrequencies_.push_back(pHin);
			relativeOutputFrequencies_.push_back(pHout);
		}
		else
		{
			cerr << "PitchTrackNote::parseXml() warning: Unknown Synth class '" << *synthClass << "'\n";
		}
		
		numSynths++;
		synthElement = synthElement->NextSiblingElement("Synth");
	}
	
	if(numSynths == 0)
		cerr << "PitchTrackNote::parseXml() warning: no Synths found\n";	
			
	return 0;
}

// This method is called with new values from the pitch tracker.  Update the synth parameters accordingly.
void PitchTrackNote::pitchTrackValues(float frequency, float amplitude)
{
	int i;
	bool shouldRelease = true;
	
	// Update all the synths
	// Use this opportunity to see if synths have released, and if all have done so, stop the note
	
	for(i = 0; i < synths_.size(); i++)
	{
		if(typeid(*synths_[i]) != typeid(PitchTrackSynth))
			continue;
		((PitchTrackSynth *)synths_[i])->setFrequencyAmplitudeData((double)frequency, (double)amplitude);
		if(!synths_[i]->isFinished())
			shouldRelease = false;
	}
	
	if(shouldRelease)
	{
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "PitchTrack note stopping\n";
#endif
		abort();
	}
}

void PitchTrackNote::abort()
{
	// Call PitchTrackController instead of MidiController to remove from list of active notes
	
	if(!isRunning_)
		return;
	
	// Tell all synths to release, and remove them from the render queue	
	// This setup doesn't allow synths any post-release activity since it removes them right away
	for(int i = 0; i < synths_.size(); i++)
	{
		synths_[i]->release();
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "removing Synth " << synths_[i] << endl;
#endif
		if(render_->removeSynth(synths_[i]))
			cerr << "PitchTrackNote::abort() warning: error removing synth #" << i << endl;
	}
	
	isRunning_ = false;	
	pitchTrackController_->noteEnded(this, key_);			// Tell the controller we finished	
}

PitchTrackNote* PitchTrackNote::createNote(int audioChannel, int mrpChannel, int midiNote, int inputMidiNote, 
										   int pianoString, unsigned int key, int priority,
										   int velocity, float phaseOffset, float amplitudeOffset)
{
	PitchTrackNote *out = new PitchTrackNote(controller_, render_, pitchTrackController_);
	int i, j;
	
	// Copy pitch tracking parameters to the new note
	
	out->setPerformanceParameters(audioChannel, mrpChannel, midiNote, 0 /* midiChannel */, pianoString, key, priority, velocity);
	
	float freq = controller_->midiNoteToFrequency(midiNote);				// Find the center frequency for this note
	float inFreq = controller_->midiNoteToFrequency(inputMidiNote);			// The note listens for input at this frequency
	
#ifdef DEBUG_MESSAGES_EXTRA
	//cout << "createNote(): centerFrequency_ = " << centerFrequency_ << endl;
#endif
	
	// Create copies of the synths in the new object, setting their appropriate center frequencies
	
	for(i = 0; i < synths_.size(); i++)
	{
		timedParameter ramp;
		parameterValue pval;
		
		if(typeid(*synths_[i]) != typeid(PitchTrackSynth))
		{
			cerr << "PTN createNote() warning: found non-PitchTrackSynth\n";
			continue;
		}
		
		PitchTrackSynth *newSynth = new PitchTrackSynth(*(PitchTrackSynth *)synths_[i]);
		if(i < relativeInputFrequencies_.size())	// Sanity check
		{
			ramp.clear();
			
			for(j = 0; j < relativeInputFrequencies_[i].ramp.size(); j++)
			{
				pval.nextValue = inFreq*relativeInputFrequencies_[i].ramp[j].nextValue;
				pval.duration = relativeInputFrequencies_[i].ramp[j].duration;
				pval.shape = relativeInputFrequencies_[i].ramp[j].shape;
				ramp.push_back(pval);
			}

			newSynth->setInputCenterFrequency(inFreq*relativeInputFrequencies_[i].start, ramp);
		}
		else
			cerr << "PTN createNote() warning: relativeInputFrequencies_ too short\n";
		
		if(i < relativeOutputFrequencies_.size())	// Sanity check
		{
			ramp.clear();
			
			for(j = 0; j < relativeOutputFrequencies_[i].ramp.size(); j++)
			{
				pval.nextValue = freq*relativeOutputFrequencies_[i].ramp[j].nextValue;
				pval.duration = relativeOutputFrequencies_[i].ramp[j].duration;
				pval.shape = relativeOutputFrequencies_[i].ramp[j].shape;
				ramp.push_back(pval);
			}
			
			newSynth->setOutputCenterFrequency(freq*relativeOutputFrequencies_[i].start, ramp);
		}
		else
			cerr << "PTN createNote() warning: relativeOutputFrequencies_ too short\n";		
		
		newSynth->setPerformanceParameters(render_->numInputChannels(), render_->numOutputChannels(), audioChannel);
		out->synths_.push_back(newSynth);
	}
	
	return out;	
}

// Private utility method that assigns parameter values to a synth based on XML data

void PitchTrackNote::assignPitchTrackSynthParameters(PitchTrackSynth *synth, 
													 paramHolder *relativeInputFreq,
													 paramHolder *relativeOutputFreq,
													 TiXmlElement *element)
{
	// Each Parameter holds three attribute tags: name, value, and (optionally) concavity for velocity-sensitive parameters
	
	const string *name = element->Attribute((const string)"name");
	const string *value = element->Attribute((const string)"value");
	const string *concavity = element->Attribute((const string)"concavity");
	
	if(value != NULL && name != NULL)	
	{
		stringstream valueStream(*value);
		vector<double> vd;
		vector<timedParameter> vtp;
		timedParameter tp;
		double c = 0.0, d;
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "assignPitchTrackSynthParameters(): Processing name = '" << (*name) << "', value = '" << (*value) << "'\n";
#endif
		if(concavity != NULL)		// Read the concavity attribute if present; if not, assume linear (0)
		{
			stringstream concavityStream(*concavity);
			concavityStream >> c;
		}
		
		// First, check that the name is a parameter we recognize.  Some will have float values, others will be a list of floats.
		// We don't look for a phase offset parameter in the XML file, since that is separately calibrated
		if(name->compare("MaxDuration") == 0) {							// double, time-invariant
			valueStream >> d;
			synth->setMaxDuration(d);
		}
		else if(name->compare("DecayTimeConstant") == 0) {				// double, time-invariant
			valueStream >> d;
			synth->setDecayTimeConstant(d);
		}
		else if(name->compare("HarmonicCentroidMultiply") == 0) {				// double, time-invariant
			bool b;
			valueStream >> boolalpha >> b;
			synth->setHarmonicCentroidMultiply(b);
		}		
		else if(name->compare("InputRelativeFrequency") == 0) {				// double, time-variant
			// Save this information separately and send it to the synth once we know which
			// note it triggers from
			valueStream >> relativeInputFreq->start;
			relativeInputFreq->ramp = parseParameterRamp(element);
		}
		else if(name->compare("OutputRelativeFrequency") == 0) {			// double, time-variant
			// Save this information separately and send it to the synth once we know which
			// note it triggers from
			valueStream >> relativeOutputFreq->start;
			relativeOutputFreq->ramp = parseParameterRamp(element);
		}		
		else if(name->compare("MaxGlobalAmplitude") == 0) {					// double, time-variant
			d = strtod_with_suffix(value->c_str());
			tp = parseParameterRamp(element);
			synth->setMaxGlobalAmplitude(d, tp);
		}
		else if(name->compare("HarmonicAmplitudes") == 0) {					// vector<double>, time-variant
			vd = parseCommaSeparatedValues(value->c_str());
			vtp = parseMultiParameterRamp(element);

			synth->setHarmonicAmplitudes(vd, vtp);
		}
		else if(name->compare("HarmonicPhases") == 0) {				// vector<double>, time-variant
			vd = parseCommaSeparatedValues(value->c_str());
			vtp = parseMultiParameterRamp(element);
			
			synth->setHarmonicPhases(vd, vtp);
		}
		else if(name->compare("InputGain") == 0) {					// double, time-variant
			d = strtod_with_suffix(value->c_str());
			tp = parseParameterRamp(element);
			synth->setInputGain(d, tp);	
		}	
		else if(name->compare("HarmonicCentroid") == 0) {
			valueStream >> d;
			tp = parseParameterRamp(element);
			synth->setHarmonicCentroid(d, tp);				
		}
		else if(name->compare("PitchFollowRange") == 0) {					// double, time-variant
			valueStream >> d;
			// TODO: convert semitone to pitch ratio
			tp = parseParameterRamp(element);
			synth->setPitchFollowRange(d, tp);	
		}				
		else if(name->compare("PitchFollowRatio") == 0) {					// double, time-variant
			valueStream >> d;
			tp = parseParameterRamp(element);
			synth->setPitchFollowRatio(d, tp);	
		}			
		else
			cerr << "assignPitchTrackSynthParameters() warning: unknown parameter '" << *name << "'\n";
	}
}


#pragma mark PitchTrackSynth

// Constructor

PitchTrackSynth::PitchTrackSynth(float sampleRate) : SynthBase(sampleRate)
{
	float defaultFreq = 440.0;
	
	// Initialize the mutex
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize parameterMutex in PitchTrackSynth\n";
		// Throw exception?
	}	
	if(pthread_mutex_init(&freqAmpMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize freqAmpMutex in PitchTrackSynth\n";
		// Throw exception?
	}		
	
	// Set defaults for changeable parameters, so we don't have to worry about allocating Parameters later
	maxDuration_ = -1.0;		// < 0 means ignore
	shouldRelease_ = false;
	decayTimeConstant_ = 0.0;
	outputCenterFrequency_ = new Parameter(defaultFreq, sampleRate);
	inputCenterFrequency_ = new Parameter(defaultFreq, sampleRate);
	inputGain_ = new Parameter(-1.0, sampleRate);	// < 0 means don't follow amplitude
	pitchFollowRange_ = new Parameter(0.0, sampleRate);
	pitchFollowRatio_ = new Parameter(0.0, sampleRate);
	harmonicCentroid_ = new Parameter(1.0, sampleRate);
	harmonicCentroidMultiply_ = false;
	
	// By default, amplitude 0.1 (-20dB) and no filters
	maxGlobalAmplitude_ = new Parameter(0.1, sampleRate);
	
	harmonicAmplitudes_.push_back(new Parameter(1.0, sampleRate)); // By default, 1 sine wave
	harmonicPhases_.push_back(new Parameter(0.0, sampleRate));
	
	// Initialize the envelope follower
	inputEnvelopeFollower_ = new EnvelopeFollower(0.0, sampleRate);	// Start with timeConstant = 0
	
	// State variables
	phase_ = 0.0;
	lastFrequency_ = defaultFreq;
	lastAmplitude_ = 0.0;
	minInputFrequency_ = maxInputFrequency_ = defaultFreq;

#ifdef DEBUG_ALLOCATION
	cout << "*** PitchTrackSynth\n";
#endif
	
}

// Copy constructor
PitchTrackSynth::PitchTrackSynth(const PitchTrackSynth& copy) : SynthBase(copy)
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** PitchTrackSynth (copy constructor)\n";
#endif
	
	// Copy all these parameters over.  First the easy ones.
	
	maxDuration_ = copy.maxDuration_;
	decayTimeConstant_ = copy.decayTimeConstant_;
	harmonicCentroidMultiply_ = copy.harmonicCentroidMultiply_;
	phase_ = copy.phase_;
	lastFrequency_ = copy.lastFrequency_;
	lastAmplitude_ = copy.lastAmplitude_;
	minInputFrequency_ = copy.minInputFrequency_;
	maxInputFrequency_ = copy.maxInputFrequency_;
	shouldRelease_ = copy.shouldRelease_;
	
	// Initialize a new mutex.  Just because the copy was locked doesn't mean this one should be.
	
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize parameterMutex in PitchTrackSynth\n";
		// Throw exception?
	}	
	if(pthread_mutex_init(&freqAmpMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize freqAmpMutex in PitchTrackSynth\n";
		// Throw exception?
	}		
	
	
	// Copy all the pointer objects
	if(copy.outputCenterFrequency_ != NULL)
		outputCenterFrequency_ = new Parameter(*copy.outputCenterFrequency_);
	else
		outputCenterFrequency_ = NULL;
	if(copy.inputCenterFrequency_ != NULL)
		inputCenterFrequency_ = new Parameter(*copy.inputCenterFrequency_);
	else
		inputCenterFrequency_ = NULL;	
	if(copy.maxGlobalAmplitude_ != NULL)
		maxGlobalAmplitude_ = new Parameter(*copy.maxGlobalAmplitude_);
	else
		maxGlobalAmplitude_ = NULL;
	if(copy.inputGain_ != NULL)
		inputGain_ = new Parameter(*copy.inputGain_);
	else
		inputGain_ = NULL;
	if(copy.inputGain_ != NULL)
		inputGain_ = new Parameter(*copy.inputGain_);
	else
		inputGain_ = NULL;
	if(copy.pitchFollowRange_ != NULL)
		pitchFollowRange_ = new Parameter(*copy.pitchFollowRange_);
	else
		pitchFollowRange_ = NULL;	
	if(copy.pitchFollowRatio_ != NULL)
		pitchFollowRatio_ = new Parameter(*copy.pitchFollowRatio_);
	else
		pitchFollowRatio_ = NULL;	
	if(copy.harmonicCentroid_ != NULL)
		harmonicCentroid_ = new Parameter(*copy.harmonicCentroid_);
	else
		harmonicCentroid_ = NULL;
	
	if(copy.inputEnvelopeFollower_ != NULL)
		inputEnvelopeFollower_ = new EnvelopeFollower(*copy.inputEnvelopeFollower_);
	else
		inputEnvelopeFollower_ = NULL;

	// Finally, copy over the vector parameters.  Fortunately, we shouldn't have NULL pointers in the
	// vectors.
	
	for(i = 0; i < copy.harmonicAmplitudes_.size(); i++)
		harmonicAmplitudes_.push_back(new Parameter(*copy.harmonicAmplitudes_[i]));
	for(i = 0; i < copy.harmonicPhases_.size(); i++)
		harmonicPhases_.push_back(new Parameter(*copy.harmonicPhases_[i]));
}

// begin() methods: we subclass these to handle maximum duration

void PitchTrackSynth::begin()
{
	SynthBase::begin();
	
	// We won't know the exact start time until the first render cycle, so we can't actually call release()
	// until then.
	
	if(maxDuration_ >= 0.0)
		shouldRelease_ = true;		// Tell this note to call release method in the render loop
}

void PitchTrackSynth::begin(PaTime when)
{
	SynthBase::begin();
	
	if(maxDuration_ >= 0.0)
		shouldRelease_ = true;
}

// Parameter Methods

void PitchTrackSynth::setMaxDuration(double maxDuration)
{
	pthread_mutex_lock(&parameterMutex_);
	
	maxDuration_ = maxDuration;
	if(maxDuration_ >= 0.0)
		shouldRelease_ = true;
		
	pthread_mutex_unlock(&parameterMutex_);	
}

void PitchTrackSynth::setDecayTimeConstant(double decayTimeConstant)
{
	pthread_mutex_lock(&parameterMutex_);	
	
	decayTimeConstant_ = decayTimeConstant;	// TODO: implement this
	inputEnvelopeFollower_->updateTimeConstant(decayTimeConstant_);
	
	pthread_mutex_unlock(&parameterMutex_);			
}

void PitchTrackSynth::setHarmonicCentroidMultiply(bool multiply)
{
	pthread_mutex_lock(&parameterMutex_);	
	
	harmonicCentroidMultiply_ = multiply;
	
	pthread_mutex_unlock(&parameterMutex_);			
}

// These methods replace the current parameters with new ones, starting immediately

void PitchTrackSynth::setInputCenterFrequency(double currentCenterFrequency, timedParameter& rampCenterFrequency)
{
	pthread_mutex_lock(&parameterMutex_);		// Acquire lock ensuring we're not actively rendering
	
	inputCenterFrequency_->setRampValues(currentCenterFrequency, rampCenterFrequency);	
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PitchTrackSynth::setOutputCenterFrequency(double currentCenterFrequency, timedParameter& rampCenterFrequency)
{
	pthread_mutex_lock(&parameterMutex_);		// Acquire lock ensuring we're not actively rendering
	
	outputCenterFrequency_->setRampValues(currentCenterFrequency, rampCenterFrequency);	
	
	pthread_mutex_unlock(&parameterMutex_);		
}

void PitchTrackSynth::setMaxGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	maxGlobalAmplitude_->setRampValues(currentAmplitude, rampAmplitude);
	
	pthread_mutex_unlock(&parameterMutex_);		
}

void PitchTrackSynth::setInputGain(double currentGain, timedParameter& rampGain)
{
	pthread_mutex_lock(&parameterMutex_);
	
	inputGain_->setRampValues(currentGain, rampGain);
	
	pthread_mutex_unlock(&parameterMutex_);		
}

void PitchTrackSynth::setPitchFollowRange(double currentRange, timedParameter& rampRange)
{
	pthread_mutex_lock(&parameterMutex_);
	
	pitchFollowRange_->setRampValues(currentRange, rampRange);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PitchTrackSynth::setPitchFollowRatio(double currentRatio, timedParameter& rampRatio)
{
	pthread_mutex_lock(&parameterMutex_);
	
	pitchFollowRatio_->setRampValues(currentRatio, rampRatio);
	
	pthread_mutex_unlock(&parameterMutex_);		
}

void PitchTrackSynth::setHarmonicCentroid(double currentCentroid, timedParameter& rampCentroid)
{
	pthread_mutex_lock(&parameterMutex_);
	
	harmonicCentroid_->setRampValues(currentCentroid, rampCentroid);
	
	pthread_mutex_unlock(&parameterMutex_);		
}


void PitchTrackSynth::setHarmonicAmplitudes(vector<double>& currentHarmonicAmplitudes,
						   vector<timedParameter>& rampHarmonicAmplitudes)
{
	// Use the currentHarmonicAmplitudes size as our metric.  If rampHarmonicAmplitudes.size is greater,
	// ignore the extra.  If it is smaller, don't ramp the last parameters.
	int i, size = currentHarmonicAmplitudes.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting amplitude
	while(size > harmonicAmplitudes_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding harmonic amplitude, size was " << harmonicAmplitudes_.size() << endl;
#endif
		harmonicAmplitudes_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
	{
		if(i < rampHarmonicAmplitudes.size())
			harmonicAmplitudes_[i]->setRampValues(currentHarmonicAmplitudes[i], rampHarmonicAmplitudes[i]);
		else
			harmonicAmplitudes_[i]->setCurrentValue(currentHarmonicAmplitudes[i]);
	}
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PitchTrackSynth::setHarmonicPhases(vector<double>& currentHarmonicPhases,
					   vector<timedParameter>& rampHarmonicPhases)
{
	// Use the currentHarmonicAmplitudes size as our metric.  If rampHarmonicAmplitudes.size is greater,
	// ignore the extra.  If it is smaller, don't ramp the last parameters.
	int i, size = currentHarmonicPhases.size();
	
	pthread_mutex_lock(&parameterMutex_);
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting amplitude
	while(size > harmonicPhases_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding harmonic phase, size was " << harmonicPhases_.size() << endl;
#endif
		harmonicPhases_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
	{
		if(i < rampHarmonicPhases.size())
			harmonicPhases_[i]->setRampValues(currentHarmonicPhases[i], rampHarmonicPhases[i]);
		else
			harmonicPhases_[i]->setCurrentValue(currentHarmonicPhases[i]);
	}
	
	pthread_mutex_unlock(&parameterMutex_);	
}

// These methods schedule new parameter additions at the end of the current ones
void PitchTrackSynth::appendInputCenterFrequency(timedParameter& centerFrequency)
{
	pthread_mutex_lock(&parameterMutex_);
	
	inputCenterFrequency_->appendRampValues(centerFrequency); 
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PitchTrackSynth::appendOuputCenterFrequency(timedParameter& centerFrequency)
{
	pthread_mutex_lock(&parameterMutex_);
	
	outputCenterFrequency_->appendRampValues(centerFrequency); 
	
	pthread_mutex_unlock(&parameterMutex_);		
}

void PitchTrackSynth::appendMaxGlobalAmplitude(timedParameter& amplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	maxGlobalAmplitude_->appendRampValues(amplitude);
	
	pthread_mutex_unlock(&parameterMutex_);		
}

void PitchTrackSynth::appendInputGain(timedParameter& inputGain)
{
	pthread_mutex_lock(&parameterMutex_);
	
	inputGain_->appendRampValues(inputGain);
	
	pthread_mutex_unlock(&parameterMutex_);			
}

void PitchTrackSynth::appendPitchFollowRange(timedParameter& range)
{
	pthread_mutex_lock(&parameterMutex_);
	
	pitchFollowRange_->appendRampValues(range);
	
	pthread_mutex_unlock(&parameterMutex_);			
}

void PitchTrackSynth::appendPitchFollowRatio(timedParameter& ratio)
{
	pthread_mutex_lock(&parameterMutex_);
	
	pitchFollowRatio_->appendRampValues(ratio);
	
	pthread_mutex_unlock(&parameterMutex_);			
}

void PitchTrackSynth::appendHarmonicCentroid(timedParameter& inputCentroid)
{
	pthread_mutex_lock(&parameterMutex_);
	
	harmonicCentroid_->appendRampValues(inputCentroid);
	
	pthread_mutex_unlock(&parameterMutex_);			
}


void PitchTrackSynth::appendHarmonicAmplitudes(vector<timedParameter>& harmonicAmplitudes)
{
	int i, size = harmonicAmplitudes.size();
	
	pthread_mutex_lock(&parameterMutex_);
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting amplitude
	while(size > harmonicAmplitudes_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding harmonic amplitude, size was " << harmonicAmplitudes_.size() << endl;
#endif
		harmonicAmplitudes_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		harmonicAmplitudes_[i]->appendRampValues(harmonicAmplitudes[i]);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PitchTrackSynth::appendHarmonicPhases(vector<timedParameter>& harmonicPhases)
{
	int i, size = harmonicPhases.size();
	
	pthread_mutex_lock(&parameterMutex_);
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting amplitude
	while(size > harmonicPhases_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding harmonic phase, size was " << harmonicPhases_.size() << endl;
#endif
		harmonicPhases_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		harmonicPhases_[i]->appendRampValues(harmonicPhases[i]);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

// This function is called by the external process that provides pitch-tracking data
// lastFrequency and lastAmplitude are used in the render loop.

void PitchTrackSynth::setFrequencyAmplitudeData(double freq, double amp)
{
	pthread_mutex_lock(&freqAmpMutex_);
	lastFrequency_ = freq;
	lastAmplitude_ = amp;
	//cout << "freq = " << freq << " amp = " << amp << endl;
	pthread_mutex_unlock(&freqAmpMutex_);
}

// Render one buffer of output.  input holds the incoming audio data.  output may already contain
// audio, so we add our result to it rather than replacing.

int PitchTrackSynth::render(const void *input, void *output, unsigned long frameCount,
					 const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags)
{
	float *outBuffer = (float *)output;		// and increment as frames are processed
	float outSample;
	PaTime bufferStartTime, bufferEndTime;
	unsigned long lastFrame = frameCount;
	unsigned long i, j;
	vector<Parameter *>::iterator it;
	bool willFinishAtEnd = false;
	
	if(!isRunning_)			// Don't do anything if the note hasn't started
		return paContinue;
	
	bufferStartTime = timeInfo->outputBufferDacTime;
	bufferEndTime = bufferStartTime + (double)frameCount*sampleLength_;	// Time this buffer will end
	
	if(startTime_ >= bufferEndTime)
		return paContinue;	// Note will begin, but not during this callback
	
	// If the note was set to begin "now", calibrate its start time to the beginning of this buffer
	// We use the start time to calculate the offset from the beginning of the note.
	if(startTime_ == 0)	
		startTime_ = bufferStartTime;
	
	if(shouldRelease_)
	{
		release(startTime_ + maxDuration_);
		shouldRelease_ = false;
	}
				
	// If the note has been set to release, check whether that should happen immediately, within this
	// buffer, or later.  If later, then go about our business like normal.
	if(isReleasing_)
	{
		if(releaseTime_ <= bufferStartTime)		// Release immediately
		{
			lastFrame = 0;
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			isFinished_ = true;
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "Releasing immediately" << endl;
#endif
		}
		else if(releaseTime_ < bufferEndTime)		// Release mid-buffer
		{
			lastFrame = (unsigned long)((releaseTime_ - bufferStartTime)*sampleRate_);
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "Releasing at sample " << lastFrame << endl;
#endif
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			willFinishAtEnd = true;				// Set the finished flag *after* rendering
		}
		// else do nothing
	}
	
	// Now calculate all the samples we need, either a full or partial frame.
	
	for(i = 0; i < lastFrame; i++)
	{
		float vcoFrequency;
		float rawOutputAmplitude, filteredOutputAmplitude;
		bool frequencyInRange = false;
		
		// Handle ramped parameter updates, but not every sample to save CPU time.
		// A parameter needs ramping when there is at least one item in its timedParameter deque.
		if(sampleNumber_ % PARAMETER_UPDATE_INTERVAL == 0)
		{
			pthread_mutex_lock(&parameterMutex_);	// Acquire lock so the parameters don't change
			
			maxGlobalAmplitude_->ramp(PARAMETER_UPDATE_INTERVAL);
			inputCenterFrequency_->ramp(PARAMETER_UPDATE_INTERVAL);
			outputCenterFrequency_->ramp(PARAMETER_UPDATE_INTERVAL);
			inputGain_->ramp(PARAMETER_UPDATE_INTERVAL);
			pitchFollowRange_->ramp(PARAMETER_UPDATE_INTERVAL);
			pitchFollowRatio_->ramp(PARAMETER_UPDATE_INTERVAL);
			harmonicCentroid_->ramp(PARAMETER_UPDATE_INTERVAL);

			minInputFrequency_ = inputCenterFrequency_->currentValue()/pitchFollowRange_->currentValue();
			maxInputFrequency_ = inputCenterFrequency_->currentValue()*pitchFollowRange_->currentValue();			
			
			for(it = harmonicAmplitudes_.begin(); it != harmonicAmplitudes_.end(); it++)
				(*it)->ramp(PARAMETER_UPDATE_INTERVAL);			
			for(it = harmonicPhases_.begin(); it != harmonicPhases_.end(); it++)
				(*it)->ramp(PARAMETER_UPDATE_INTERVAL);
			
			pthread_mutex_unlock(&parameterMutex_);
		}	
		
		// Lock this mutex during the calculations that use lastFrequency and lastAmplitude.  We expect that
		// these values might change from one frame to the next, so we don't want to use the coarser-grained
		// parameter mutex.
		
		// FIXME: Render actually happens a good deal before the audio comes out, and the timing isn't specified.
		// The interaction of threads here might produce somewhat strange results....
		
		pthread_mutex_lock(&freqAmpMutex_);		
	
		if(lastFrequency_ > minInputFrequency_ && lastFrequency_ < maxInputFrequency_)
			frequencyInRange = true;
		
		// Calculate the output frequency, which follows the input unless the input is out of range (or
		// unless we've disabled this feature
		
		if(pitchFollowRatio_->currentValue() != 0.0 && frequencyInRange)
		{
			float outInRatio;
			
			// We want one semitone in to mean one semitone out in deviation, but that will require scaling
			// for the respective registers of the tones
			
			if(inputCenterFrequency_->currentValue() != 0.0)		
				outInRatio = outputCenterFrequency_->currentValue() / inputCenterFrequency_->currentValue();
			else
				outInRatio = 1.0;

			vcoFrequency = outputCenterFrequency_->currentValue() + 
								(lastFrequency_ - inputCenterFrequency_->currentValue())*pitchFollowRatio_->currentValue()*outInRatio;
		}
		else
			vcoFrequency = outputCenterFrequency_->currentValue();	// Stay with center frequency
			   
		// Calculate the output amplitude, which depends on these factors:
		//   (1) The strength of the input signal, multiplied by the input gain
		//   (2) The maximum amplitude (clips at this value)
		//   (3) If pitchFollowRange != 1.0, then the amplitude rolls off linearly away from the
		//       center frequency.  That's so we don't get weird clicks and pops as the input signal passes in
		//       and out of range.
		
		if(inputGain_->currentValue() >= 0.0)
		{
			rawOutputAmplitude = lastAmplitude_*inputGain_->currentValue();
			if(rawOutputAmplitude > maxGlobalAmplitude_->currentValue())
				rawOutputAmplitude = maxGlobalAmplitude_->currentValue();
		}
		else
			rawOutputAmplitude = maxGlobalAmplitude_->currentValue();
		
		// TODO: averaging of input frequencies?
		
		if(minInputFrequency_ < maxInputFrequency_ && inputGain_->currentValue() >= 0.0)	// True if pitchFollowRange > 1.0
		{
			// Calculate the distance from centerFrequency to the current frequency, and do a linear rolloff in
			// amplitude.  In perceptual (logarithmic) space, the rolloff will be the steepest toward the edges,
			// which is what we want.
			
			// FIXME: if this works, the dividers can be put into state variables and updated less frequently
			
			if(!frequencyInRange)
				rawOutputAmplitude = 0.0;
			else
			{
				if(lastFrequency_ <= inputCenterFrequency_->currentValue())
				{
					rawOutputAmplitude *= (lastFrequency_ - minInputFrequency_) / (inputCenterFrequency_->currentValue() - minInputFrequency_);
				}
				else
				{				
					rawOutputAmplitude *= (maxInputFrequency_ - lastFrequency_) / (maxInputFrequency_ - inputCenterFrequency_->currentValue());
				}
			}
		}

		// Unlock the frequency/amplitude mutex
		pthread_mutex_unlock(&freqAmpMutex_);	
		
		if(inputGain_->currentValue() >= 0.0)
		{
			filteredOutputAmplitude = inputEnvelopeFollower_->filter(rawOutputAmplitude);
			if(filteredOutputAmplitude > maxGlobalAmplitude_->currentValue())
				filteredOutputAmplitude = maxGlobalAmplitude_->currentValue();
		}
		else
			filteredOutputAmplitude = rawOutputAmplitude; // < 0 means bypass the filter process
		
		// Increment the phase according to the current frequency
		phase_ = fmod(phase_ + vcoFrequency*sampleLength_, 1.0);

		// Calculate the output as a sum of sine waves at each harmonic
		outSample = 0.0;

		// Harmonic amplitudes refer directly to the strength of each partial at the output
		for(j = 0; j < harmonicAmplitudes_.size(); j++)
		{
			if(harmonicAmplitudes_[j]->currentValue() == 0.0)
				continue;

			float hPhase = (j < harmonicPhases_.size() ? harmonicPhases_[j]->currentValue() : 0.0);
			
			if(harmonicCentroid_->currentValue() == 1.0)
			{
				outSample += (float)(harmonicAmplitudes_[j]->currentValue())
							* waveTable.lookupInterp(waveTableSine, phase_*(float)(j+1)+hPhase);
			}
			else 
			{
				// Calculate the actual contributions of this harmonic based on the centroid, which
				// could shift or scale its frequency value.
				
				double num;
				
				if(harmonicCentroidMultiply_)
					num = (j+1) * harmonicCentroid_->currentValue();
				else
					num = (j+1) + (harmonicCentroid_->currentValue() - 1.0);					

				// val will most probably lie between two integers, in which case we assign each of its neighbors a weighted
				// sum.  Keep some sort of limit on how many harmonics can be defined this way, so the system doesn't get
				// too too slow.
				
				if(num == floor(num))	// num is a pure integer
				{
					outSample += (float)(harmonicAmplitudes_[j]->currentValue())
								  * waveTable.lookupInterp(waveTableSine, phase_*num+hPhase);
				}
				else					// num has a fractional component
				{
					// harmonic below
					outSample += (float)(harmonicAmplitudes_[j]->currentValue()) * (ceil(num) - num)
									* waveTable.lookupInterp(waveTableSine, phase_*floor(num)+hPhase);
					outSample += (float)(harmonicAmplitudes_[j]->currentValue()) * (num - floor(num))
									* waveTable.lookupInterp(waveTableSine, phase_*ceil(num)+hPhase);					
				}				
			}
		}
		
		// Mix the output into the buffer, scaling by the global amplitude
		outBuffer[outputChannel_] += outSample*filteredOutputAmplitude;
		
#ifdef DEBUG_MESSAGES_EXTRA
		if(sampleNumber_ % DEBUG_MESSAGE_SAMPLE_INTERVAL == 0)
		{
			cout << "outBuffer = " << outBuffer << " channel = " << outputChannel_ << endl;
			cout << "freq = " << vcoFrequency << ", lastFreq = " << lastFrequency_ << ", output amp = " << filteredOutputAmplitude << endl;
		}
#endif
		
		// Update counters for next cycle
		sampleNumber_++;
		outBuffer += numOutputChannels_;
	}
	
	if(willFinishAtEnd)
		isFinished_ = true;
	
	return paContinue;
}


PitchTrackSynth::~PitchTrackSynth()
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** ~PitchTrackSynth\n";
#endif
	
	// Parameters are dynamically-allocated and need to be deleted:
	
	delete inputCenterFrequency_;
	delete outputCenterFrequency_;
	delete inputGain_;
	delete pitchFollowRange_;
	delete pitchFollowRatio_;
	delete harmonicCentroid_;
	
	for(i = 0; i < harmonicAmplitudes_.size(); i++)
		delete harmonicAmplitudes_[i];
	for(i = 0; i < harmonicPhases_.size(); i++)
		delete harmonicPhases_[i];
	
	delete maxGlobalAmplitude_;
	delete inputEnvelopeFollower_;

	pthread_mutex_destroy(&parameterMutex_);	
	pthread_mutex_destroy(&freqAmpMutex_);
}
