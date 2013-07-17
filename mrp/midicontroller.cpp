/*
 *  midicontroller.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/26/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include <cstdlib>
#include <sstream>
#include <fstream>
#include "errno.h"
#include "midicontroller.h"
#include "pitchtrack.h"
#include "config.h"
#include "note.h"
#include "realtimenote.h"
#include "pnoscancontroller.h"

#define DEBUG_MESSAGES_RAW_MIDI

#define PROGRAM_ID(prog, chan, note) (unsigned int)((prog << 12) + (chan << 8) + note)

#define INVERT_SOSTENUTO_PEDAL 0
#define INVERT_DAMPER_PEDAL 0

extern char *kNoteNames[];

MidiController::MidiController(AudioRender *render)
{
	render_ = render;	// Hold a reference to the audio rendering engine
	midiOut_ = NULL;	// Don't yet have an output device or the pitch tracker
	pitchTrackController_ = NULL;
	mrpChannel_ = 0;
	
	// Initialize the mutex
	if(pthread_mutex_init(&eventMutex_, NULL) != 0)
	{
		cerr << "Warning: MidiController failed to initialize event mutex\n";
		// Throw exception?
	}
	
	bzero(inputControllers_, 16*128*sizeof(unsigned char));	// Set default values
	//bzero(inputPatches_, 16*sizeof(unsigned char));
	currentProgram_ = 0;
	a4Tuning_ = 440.0;
	
	for(int i = 0; i < 16; i++)
	{
		inputPitchWheels_[i] = 0x2000;	// Center value for each pitch wheel
		canTriggerNoteOnChannel_[i] = true;	// All channels trigger notes by default
	}
	for(int i = 0; i < 128; i++)
	{
		stringNoteMaps_[i] = i;					// By default, every note sounds on its own string
		pianoDamperStates_[i] = DAMPER_DOWN;	// No damper lifting initially
		
		// Initialize the tuning table and set the offsets
		noteFrequencies_[i] = a4Tuning_*(float)pow(2.0, ((float)i-69.0)/12.0);
		phaseOffsets_[i] = 0.0;
		amplitudeOffsets_[i] = 1.0;
	}
	
	controlMasterVolume_ = -1;			// Not enabled by default
	controlPitchTrackInputMute_ = -1;
	controlPitchTrackInputMuteThresh_ = 0;
	lastPitchTrackInputMute_ = false;
	displaceOldNotes_ = false;
	lastCalibrationFile_ = "";
	
	// Start the cleanup thread which checks for finished notes
	cleanupShouldTerminate_ = false;
	
	if(pthread_create(&cleanupThread_, NULL, cleanupLoop, this) != 0)
	{
		cerr << "Warning: Error creating cleanup thread!  Notes will not auto-terminate.\n";
	}
}

void MidiController::setA4Tuning(float tuning)
{
	a4Tuning_ = tuning;
	
	for(int i = 0; i < 128; i++)
		noteFrequencies_[i] = a4Tuning_*(float)pow(2.0, ((float)i-69.0)/12.0);
}

// Load patch table data from an XML file.

int MidiController::loadPatchTable(string& filename)
{
	TiXmlDocument doc(filename);
	TiXmlElement *baseElement, *element;
	map<string, Note*>::iterator oldPatchIterator;
	int lastProgramId = -1;
	
	// Clear out the old patches to make room for the new
	for(oldPatchIterator = patches_.begin(); oldPatchIterator != patches_.end(); oldPatchIterator++)
	{
		delete oldPatchIterator->second;
	}
	patches_.clear();
	programs_.clear();
	programTriggeredChanges_.clear();
	if(pitchTrackController_ != NULL)
	{
		pitchTrackController_->programs_.clear();
		pitchTrackController_->notesToTurnOff_.clear();
		pitchTrackController_->notesToTurnOn_.clear();
	}
	
	if(!doc.LoadFile())
	{
		cerr << "Unable to load patch table file: \"" << filename << "\". Error was:\n";
		cerr << doc.ErrorDesc() << " (Row " << doc.ErrorRow() << ", Col " << doc.ErrorCol() << ")\n";
		return 1;
	}
	
	baseElement = doc.FirstChildElement("PatchTableRoot");
	if(baseElement == NULL)
	{
		cerr << "loadPatchTable(): Could not find PatchTableRoot element!\n";
		return 1;
	}
	
	// Go through and load each patch.  Each one is enclosed in a <Patch> tag, which has a name
	// attribute and a class attribute.  Initialize a new member of the given class and pass the
	// rest of the parsing off to it, since it will be class-specific
	
	if((element = baseElement->FirstChildElement("Patch")) == NULL)
	{
		cerr << "loadPatchTable(): No patches found!\n";
		return 1;
	}
	while(element != NULL)	// Step through all elements of type Patch
	{
		const string *patchName = element->Attribute((const string)"name");
		const string *patchClass = element->Attribute((const string)"class");
		
		if(patchName != NULL && patchClass != NULL)
		{
			// Eventually do this dynamically?
			if(patchClass->compare("MidiNote") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Parsing MidiNote, name " << *patchName << endl;
#endif
				MidiNote *note = new MidiNote(this, render_);
				note->parseXml(element);
				
				patches_[(*patchName)] = note;			// Store this for future use
			}
			else if(patchClass->compare("RealTimeMidiNote") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Parsing RealTimeMidiNote, name " << *patchName << endl;
#endif
				RealTimeMidiNote *note = new RealTimeMidiNote(this, render_);
				note->parseXml(element);
				
				patches_[(*patchName)] = note;			// Store this for future use
			}
			else if(patchClass->compare("CalibratorNote") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Parsing CalibratorNote, name " << *patchName << endl;
#endif
				CalibratorNote *note = new CalibratorNote(this, render_);
				note->parseXml(element);
				
				patches_[(*patchName)] = note;			// Store this for future use
			}
			else if(patchClass->compare("PitchTrackNote") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Parsing PitchTrackNote, name " << *patchName << endl;
#endif
				PitchTrackNote *note = new PitchTrackNote(this, render_, pitchTrackController_);
				note->parseXml(element);
				
				patches_[(*patchName)] = note;			// Store this for future use
			}
			else if(patchClass->compare("ResonanceNote") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Parsing ResonanceNote, name " << *patchName << endl;
#endif
				ResonanceNote *note = new ResonanceNote(this, render_);
				note->parseXml(element);
				
				patches_[(*patchName)] = note;			// Store this for future use
			}
			else
			{
				cerr << "loadPatchTable() warning: unknown class \"" << *patchClass << "\"\n";
			}
		}
		else
			cerr << "loadPatchTable() warning: <Patch> must have name and class attributes, skipping\n";
        
		element = element->NextSiblingElement("Patch");			// Advance to the next patch
	}
	if(patches_.size() == 0)									// Make sure we picked up at least one usable patch
	{
		cerr << "loadPatchTable(): No valid patches found!\n";
		return 1;
	}
	
	// Having loaded the patches themselves, now we need to load the patch table, which maps MIDI Program numbers to
	// patches.  The contents of the <PatchTable> element tell us how to do this.
	
	if((element = baseElement->FirstChildElement("PatchTable")) == NULL)
	{
		cerr << "loadPatchTable(): No patch table found!\n";
		return 1;
	}
	if((element = element->FirstChildElement("Program")) == NULL)
	{
		cerr << "loadPatchTable(): No programs found in patch table!\n";
		return 1;
	}
	while(element != NULL)			// Go through and set up each program
	{
		// This is something of an abuse of the MIDI Program Change specification in that we change programs for multiple
		// channels from a single Program Change message.  However, for the immediate moment, it's important that we define
		// complete interfaces for the performer which encompass the actions of both keyboards.
		
		int programId;
		
		if(element->QueryIntAttribute("id", &programId) != TIXML_SUCCESS)
		{
			programId = -1;
			// It's possible we have a non-numeric character here.  Match "+" to mean "one higher than the last program"
			const string *pidString = element->Attribute((const string)"id");
			if(pidString != NULL)
			{
				if(*pidString == (const string)"+")
				{
					cout << "loadPatchTable(): id=\"+\" assigned program ID " << lastProgramId + 1 << endl;
					programId = lastProgramId + 1;
				}
			}
			
			if(programId == -1)
				cerr << "loadPatchTable() warning: Could not find ID of program, skipping.\n";
		}
		
		if(programId != -1)
		{
			if(programId < 0 || programId > 127)
				cerr << "loadPatchTable() warning: Invalid program ID " << programId << ", skipping.\n";
			else
			{
				lastProgramId = programId;
				
				TiXmlElement *channelElement = element->FirstChildElement("Channel");
				while(channelElement != NULL)	// Iterate through the channels
				{
					TiXmlHandle channelHandle(channelElement);
					TiXmlText *text;
					int channelID;
					string patchName;
					ProgramInfo info;
					bool useHighVelocity, useLowVelocity;
					bool useAuxOn, useAuxOff;
					bool foundNote = false;
					
					// Step 0: Get channel ID attribute
					if(channelElement->QueryIntAttribute("id", &channelID) != TIXML_SUCCESS)
						cerr << "loadPatchTable() warning: Could not find ID of channel, skipping.\n";
					else
					{
						TiXmlElement *patchElement = channelElement->FirstChildElement("Patch");
						info.notes[0] = info.notes[1] = info.notes[2] = info.notes[3] = NULL;
						info.useAuxPedal = false;
                        
						// Step 0.5: Check if we use velocity splitting
                        
						if(channelElement->QueryIntAttribute("velocitySplitPoint", &info.velocitySplitPoint) != TIXML_SUCCESS)
							info.velocitySplitPoint = -1;
                        
						while(patchElement != NULL)
						{
							TiXmlHandle patchHandle(patchElement);
							useAuxOn = useAuxOff = useHighVelocity = useLowVelocity = false;
							
							// Step 1: Get Patch name for this channel
							text = patchHandle.FirstChild().ToText();
							if(text != NULL)
							{
								if(info.velocitySplitPoint > 0)	// If the velocitySplitPoint attribute is active, choose different patches
								{							// based on note velocity
									const string *velString = patchElement->Attribute((const string)"velocity");
									if(velString != NULL)
									{
										if(*velString == (const string)"high")
											useHighVelocity = true;				// Save high velocity slots only
										else
											useLowVelocity = true;				// Save low velocity slots only
									}
									else
										useHighVelocity = useLowVelocity = true;	// Save in both high and low velocity slots
								}
								else
									useHighVelocity = useLowVelocity = true;
								const string *auxString = patchElement->Attribute((const string)"aux");
								if(auxString != NULL)	// Step 1.33: check if we use the aux pedal to split patches
								{
									bool b;
									stringstream s(*auxString);
									s >> boolalpha >> b;
									
									info.useAuxPedal = true;
									
									if(b)
										useAuxOn = true;		// '... aux="true"'
									else
										useAuxOff = true;		// '... aux="false"'
								}
								else
									useAuxOn = useAuxOff = true;	// no aux attribute at all, save in both slots
								
								// Step 1.66: save this patch in the correct slot(s)
								
								patchName = text->ValueStr();
#ifdef DEBUG_MESSAGES
								cout << "Program ID " << programId << ": Patch " << patchName << endl;
#endif
								Note *noteToSave = patches_[text->ValueStr()];		// Look up in patch table
								
								if(noteToSave != NULL)
								{
									foundNote = true;
									
									if(useAuxOff && useLowVelocity)
									{
										info.notes[0] = noteToSave;
										//cout << "Adding " << patchName << " to low velocity, aux off\n";
									}
									if(useAuxOff && useHighVelocity)
									{
										info.notes[1] = noteToSave;
										//cout << "Adding " << patchName << " to high velocity, aux off\n";
									}
									if(useAuxOn && useLowVelocity)
									{
										info.notes[2] = noteToSave;
										//cout << "Adding " << patchName << " to low velocity, aux on\n";
									}
									if(useAuxOn && useHighVelocity)
									{
										info.notes[3] = noteToSave;
										//cout << "Adding " << patchName << " to high velocity, aux on\n";
									}
								}
							}
							
							patchElement = patchElement->NextSiblingElement("Patch");
						}
						
						if(foundNote)
						{
							// Step 2: Get damper and sostenuto pedal parameters.  These say whether the note responds
							// to those pedals or not.  Both parameters are optional.
							
							info.useDamperPedal = false;
							info.useSostenutoPedal = true;
                            info.sustainAlways = false;
                            info.retriggerEachPress = true;
							info.priority = 0;
                            info.monoVoice = -1;
							
							text = channelHandle.FirstChildElement("UseDamperPedal").FirstChild().ToText();
							if(text != NULL)
							{
								stringstream s(text->ValueStr());
								s >> boolalpha >> info.useDamperPedal;
#ifdef DEBUG_MESSAGES_EXTRA
								cout << "loadPatchTable(): damper bool value read as '" << info.useDamperPedal << "'\n";
#endif
							}
							text = channelHandle.FirstChildElement("UseSostenutoPedal").FirstChild().ToText();
							if(text != NULL)
							{
								stringstream s(text->ValueStr());
								s >> boolalpha >> info.useSostenutoPedal;
#ifdef DEBUG_MESSAGES_EXTRA
								cout << "loadPatchTable(): sostenuto bool value read as '" << info.useSostenutoPedal << "'\n";
#endif
							}
                            text = channelHandle.FirstChildElement("SustainAlways").FirstChild().ToText();
							if(text != NULL)
							{
								stringstream s(text->ValueStr());
								s >> boolalpha >> info.sustainAlways;
#ifdef DEBUG_MESSAGES_EXTRA
								cout << "loadPatchTable(): sustainAlways bool value read as '" << info.sustainAlways << "'\n";
#endif
							}
                            text = channelHandle.FirstChildElement("RetriggerEachPress").FirstChild().ToText();
							if(text != NULL)
							{
								stringstream s(text->ValueStr());
								s >> boolalpha >> info.retriggerEachPress;
#ifdef DEBUG_MESSAGES_EXTRA
								cout << "loadPatchTable(): retriggerEachPress bool value read as '" << info.retriggerEachPress << "'\n";
#endif
							}
							
							// Step 2.5: Get note priority (optional parameter).  If not specified, use 0.
							
							text = channelHandle.FirstChildElement("Priority").FirstChild().ToText();
							if(text != NULL)
							{
								stringstream s(text->ValueStr());
								s >> info.priority;
							}
                            
                            // Step 2.75: Get note mono voice, if specified-- used to specify a group of notes
                            //  only one of which can sound at a time.
                            
                            text = channelHandle.FirstChildElement("MonoVoice").FirstChild().ToText();
                            if(text != NULL)
                            {
  								stringstream s(text->ValueStr());
								s >> info.monoVoice;
                                if(info.monoVoice >= 16 || info.monoVoice < 0)
                                    info.monoVoice = -1;
                            }
                            
                            // Step 2.8: Get the (optional) amplitude offset (volume adjustment) for this program,
                            // which can be either a single number or two numbers indicating a linear range from
                            // low to high.
                            
                            text = channelHandle.FirstChildElement("RelativeVolume").FirstChild().ToText();
                            if(text != NULL)
                            {
                            	char *str, *strOrig, *ap = NULL, *ap2 = NULL;
                                int count = 0;
                                double out[2];
                                
                                str = strOrig = strdup(text->ValueStr().c_str());
                                
                                if((ap = strsep(&str, "/")) != NULL)	// Look for up to two tokens
                                    ap2 = strsep(&str, "/");
                                
                                if(ap != NULL) {
                                    if((*ap) != '\0') {
                                        char *endString;
                                        
                                        out[count] = strtod(ap, &endString);
                                        if(!strncmp(endString, "dB", 2) || !strncmp(endString, "db", 2))
                                        {
                                            out[count] = pow(10.0, out[count]/20.0);
                                            // Convert dB to linear scale
                                        }
                                        
                                        count++;
                                    }
                                }
                                
                                if(ap2 != NULL) {
                                    if((*ap2) != '\0') {
                                        char *endString;
                                        
                                        out[count] = strtod(ap2, &endString);
                                        if(!strncmp(endString, "dB", 2) || !strncmp(endString, "db", 2))
                                        {
                                            out[count] = pow(10.0, out[count]/20.0);
                                            // Convert dB to linear scale
                                        }
                                        
                                        count++;
                                    }
                                }
                                
                                if(count == 2)
                                {
                                    info.amplitudeOffset = out[0]; // FIXME: use a range
                                }
                                else if(count == 1)
                                {
                                    info.amplitudeOffset = out[0];
                                }
                                else
                                    info.amplitudeOffset = 1.0;
                                
                                free(strOrig);	// This pointer won't move like str
                            }
                            else
                                info.amplitudeOffset = 1.0;
							
							// Step 3: Get key range (optional parameter).  If not specified, use whole keyboard range
							
							text = channelHandle.FirstChildElement("Range").FirstChild().ToText();
							vector<int> noteNumbers;
							int i;
							
							if(text != NULL)
								noteNumbers = parseRangeString(text->ValueStr());
							
							if(noteNumbers.size() > 0)
							{
								vector<int>::iterator it;
								for(it = noteNumbers.begin(); it != noteNumbers.end(); it++)
								{
									if((*it) < 0 || (*it) > 127)
										continue;
									programs_[PROGRAM_ID(programId, channelID, *it)] = info;
#ifdef DEBUG_MESSAGES_EXTRA
									cout << "loadPatchTable(): Added patch " << patchName << " to Ch" << channelID << " note " << (*it) << endl;
#endif
								}
							}
							else	// If no range was defined, use the complete range
							{
								for(i = 0; i < 128; i++)
									programs_[PROGRAM_ID(programId, channelID, i)] = info;
#ifdef DEBUG_MESSAGES_EXTRA
								cout << "loadPatchTable(): Added patch " << patchName << " to Ch" << channelID << " all notes" << endl;
#endif
							}
						}
					}
					channelElement = channelElement->NextSiblingElement("Channel");
				}
				
				if(pitchTrackController_ != NULL)
				{
					TiXmlElement *pitchTrackElement = element->FirstChildElement("PitchTrack");
					
					while(pitchTrackElement != NULL)
					{
						pitchTrackController_->parsePatchTable(pitchTrackElement, programId);
						pitchTrackElement = pitchTrackElement->NextSiblingElement("PitchTrack");
					}
				}
			}
		}
		
		// Look for elements telling us when to update the program (triggered by keyboard activity).
		
		TiXmlElement *programChangeElement = element->FirstChildElement("ProgramChange");
		
		while(programChangeElement != NULL)
		{
			const string *pcChannelString = programChangeElement->Attribute((const string)"channel");
			const string *pcNoteString = programChangeElement->Attribute((const string)"note");
			const string *pcProgramString = programChangeElement->Attribute((const string)"program");
			
			if(pcChannelString != NULL && pcNoteString != NULL & pcProgramString != NULL)
			{
				int iChannel, iNote, iNewProgram;
				
				stringstream sChannel(*pcChannelString);
				stringstream sNote(*pcNoteString);
				
				sChannel >> iChannel;
				sNote >> iNote;
				
				if(iNote > 0)
				{
                    
					if(*pcProgramString == "+")
						iNewProgram = programId + 1;
					else if(*pcProgramString == "-")
						iNewProgram = programId - 1;
					else
					{
						stringstream sProgram(*pcProgramString);
						sProgram >> iNewProgram;
					}
					
					programTriggeredChanges_[PROGRAM_ID(programId, iChannel, iNote)] = iNewProgram;
				}
			}
			
			programChangeElement = programChangeElement->NextSiblingElement("ProgramChange");
		}
		
		element = element->NextSiblingElement("Program");
	}
	
	// Parse the string map, which tells us which MIDI note should direct to which string.  It isn't always a 1-1 mapping because
	// there may not be actuators on every string.  Presently, this is a global mapping that affects all programs from all keyboards.
	
	if((element = baseElement->FirstChildElement("StringMap")) != NULL)
	{
		int i;
		
		for(i = 0; i < 128; i++)		// Start off with default mapping
			stringNoteMaps_[i] = i;
		
		element = element->FirstChildElement("Map");
		while(element != NULL)
		{
			const string *str = element->Attribute((string)"note");
			const string *str2 = element->Attribute((string)"string");
            
			if(str != NULL && str2 != NULL)
			{
#ifdef DEBUG_MESSAGES
				cout << "<Map>: note = '" << *str << "' string = '" << *str2 << "'\n";
#endif
				vector<int> noteRange = parseRangeString(*str);
				vector<int> stringRange = parseRangeString(*str2);
				
				if(noteRange.size() != stringRange.size())	// Check that the ranges line up in size
				{
					cerr << "loadPatchTable() warning: In StringMap, note and string range sizes don't match, skipping\n";
				}
				else
				{
					for(i = 0; i < noteRange.size(); i++)
					{
						if(noteRange[i] < 0 || noteRange[i] > 127 || stringRange[i] < 0 || stringRange[i] > 127)
							continue;
						stringNoteMaps_[noteRange[i]] = stringRange[i];
#ifdef DEBUG_MESSAGES_EXTRA
						cout << "Mapping note " << noteRange[i] << " to string " << stringRange[i] << endl;
#endif
					}
				}
			}
			else
			{
				cerr << "loadPatchTable() warning: Range element missing 'note' or 'string' attributes\n";
			}
            
			element = element->NextSiblingElement("Map");
		}
	}
	
	// Load a few controllers that have global settings, such as master volume
	if((element = baseElement->FirstChildElement("GlobalControls")) != NULL)
	{
		TiXmlElement *controlElement = element->FirstChildElement("Control");
		
		while(controlElement != NULL)
		{
			const string *controlName = controlElement->Attribute((string)"name");
			const string *controlId = controlElement->Attribute((string)"id");
			
			if(controlName != NULL && controlId != NULL)
			{
				if(*controlName == (const string)"MasterVolume")
				{
					stringstream s(*controlId);
					
					s >> controlMasterVolume_;
					cout << "Master Volume on controller " << controlMasterVolume_ << endl;
				}
				else if(*controlName == (const string)"PitchTrackInputMute")
				{
					stringstream s(*controlId);
					s >> controlPitchTrackInputMute_;
                    
					const string *controlThreshold = controlElement->Attribute((string)"threshold");
					
					if(controlThreshold != NULL)
					{
						stringstream s2(*controlThreshold);
						s2 >> controlPitchTrackInputMuteThresh_;
					}
					else
						controlPitchTrackInputMuteThresh_ = 16;
					
					cout << "Pitch-Track Input Mute on controller " << controlPitchTrackInputMute_ << ", threshold " << controlPitchTrackInputMuteThresh_ << endl;
				}
			}
			
			controlElement = controlElement->NextSiblingElement();
		}
	}
	
	if(pitchTrackController_ != NULL)
		pitchTrackController_->parseGlobalSettings(baseElement);
	
	return 0;
}

int MidiController::loadCalibrationTable(string& filename)
{
	int errorNumber = 0;
	
	try
	{
		// Format: "[note#] [phase] [amplitude]"
		// Parse the calibration text table
		ifstream inputFile;
		int note;
		
		inputFile.open(filename.c_str(), ios::in);
		if(inputFile.fail())		// Failed to open file...
			return 1;
		for(int i = 0; i < 128; i++)
		{
			inputFile >> note;
			if(note != i)
			{
				cerr << "loadCalibrationTable(): Expected note " << i << ", found " << note << endl;
				errorNumber = 1;
				break;
			}
			inputFile >> phaseOffsets_[i];
			inputFile >> amplitudeOffsets_[i];
		}
		inputFile.close();
		lastCalibrationFile_ = filename;
	}
	catch(...)
	{
		return 1;
	}
    
	return errorNumber;
}

int MidiController::saveCalibrationTable(string& filename)
{
	try
	{
		// Save the calibration to a text file
		// Format: "[note#] [phase] [amplitude]"
		ofstream outputFile;
        
		outputFile.open(filename.c_str(), ios::out);
		for(int i = 0; i < 128; i++)
		{
			outputFile << i << " " << phaseOffsets_[i] << " " << amplitudeOffsets_[i] << endl;
		}
		outputFile.close();
		lastCalibrationFile_ = filename;
	}
	catch(...)
	{
		return 1;
	}
    
	return 0;
}

void MidiController::clearCalibration()
{
	for(int i = 0; i < 128; i++)
	{
		phaseOffsets_[i] = 0.0;
		amplitudeOffsets_[i] = 1.0;
	}
}

void MidiController::allNotesOff(int midiChannel)			// midiChannel == -1 ---> reset all channels
{
	// Send note-off message to everything in the currentNotes collection
	
	map<unsigned int, Note*>::iterator it = currentNotes_.begin();
	Note *oldNote;
	
	//for(it = currentNotes_.begin(); it != currentNotes_.end(); it++)
	while(it != currentNotes_.end())
	{
		unsigned int key = (*it).first;
		unsigned int channel = ((*it).second)->midiChannel();
#ifdef DEBUG_MESSAGES
		cout << "allNotesOff(): found note with key " << key << ", channel " << channel << endl;
#endif
		if(channel == midiChannel || midiChannel < 0)
		{
			oldNote = (*it++).second;
            
			if(midiChannel != -1)
				mrpSendRoutingMessage(oldNote->mrpChannel(), 0);		// Disconnect the signal router
			oldNote->abort();					// Forcibly stop the note
			if(currentNotes_.count(key) > 0)	// If the Note object has not removed itself during abort(), remove it from the map
			{
				removeEventListener(oldNote);	// Remove the note from any event listeners
				currentNotes_.erase(key);
				delete oldNote;
			}
		}
		else
			++it;
	}
	
	if(midiChannel == -1)
		mrpClearRoutingTable();					// Clear all signal router connections
}

void MidiController::allControllersOff(int midiChannel)		// midiChannel == -1 ---> reset all channels
{
	// Reset all controllers.  This may include the primary piano pedals, so we need to handle those appropriately
	
	if(midiChannel <= 0 || midiChannel > 15)	// Channel 0, or all channels
	{
		sostenutoPedalChange(0);
		damperPedalChange(0);
	}
	if(midiChannel < 0 || midiChannel > 15)
		bzero(inputControllers_, 16*128*sizeof(unsigned char));	// Set default values for all controllers
	else
	{
		// Question: does this zero out the right controller values?  How is the array stored internally?
		bzero(inputControllers_[midiChannel], 128*sizeof(unsigned char));	// Set default values for this controller
	}
}

void MidiController::clearInputState()
{
	// Delete all controller and patch information, but don't change the size of the storage bins
	
	allNotesOff(-1);
	allControllersOff(-1);		// This will reset pianoDamperStates_
	
	//bzero(inputPatches_, 16*sizeof(unsigned char));		// Reset patches
	currentProgram_ = 0;
	oscSendPatchValue();
	
	for(int i = 0; i < 16; i++)
		inputPitchWheels_[i] = 0x2000;	// Center value for each pitch wheel
}

// The following three functions are called in response to a console command.
// They lock the mutex to avoid any MIDI events interrupting execution; therefore
// they should never be called internally from the MIDI thread or deadlock will result.

void MidiController::consoleProgramChange(int program)
{				// Externally execute a program change (e.g. from the console)
	pthread_mutex_lock(&eventMutex_);
	
	if(program >= 0 && program < 128)
		currentProgram_ = program;
	if(pitchTrackController_ != NULL)
		pitchTrackController_->programChanged();
	
	pthread_mutex_unlock(&eventMutex_);
	oscSendPatchValue();
}

int MidiController::consoleProgramIncrement()
{						// Increment the program by one (convenience method for performance)
	pthread_mutex_lock(&eventMutex_);
	
	currentProgram_++;
	if(currentProgram_ > 127)
		currentProgram_ = 0;
	if(pitchTrackController_ != NULL)
		pitchTrackController_->programChanged();
	
	pthread_mutex_unlock(&eventMutex_);
	oscSendPatchValue();
	return currentProgram_;
}

int MidiController::consoleProgramDecrement()
{
	pthread_mutex_lock(&eventMutex_);
	
	currentProgram_--;
	if(currentProgram_ < 0 || currentProgram_ > 127)
		currentProgram_ = 127;
	if(pitchTrackController_ != NULL)
		pitchTrackController_->programChanged();
	
	pthread_mutex_unlock(&eventMutex_);
	oscSendPatchValue();
	return currentProgram_;
}

void MidiController::consoleAllNotesOff(int midiChannel)
{
	pthread_mutex_lock(&eventMutex_);
	allNotesOff(midiChannel);
	pthread_mutex_unlock(&eventMutex_);
}


unsigned char MidiController::getControllerValue(int channel, int controller)
{
	if(channel < 0) channel = 0;
	if(channel > 15) channel = 15;
	if(controller < 0) controller = 0;
	if(controller > 127) controller = 127;
	
	return inputControllers_[channel][controller];
}

unsigned char MidiController::getPatchValue(int channel)
{
	if(channel < 0) channel = 0;
	if(channel > 15) channel = 15;
	
	//return inputPatches_[channel];
	return currentProgram_;				// For now, we use a system where every channel has the same program
}

unsigned int MidiController::getPitchWheelValue(int channel)
{
	if(channel < 0) channel = 0;
	if(channel > 15) channel = 15;
	
	return inputPitchWheels_[channel];
}

void MidiController::setNoteDisabledChannels(vector<int>& channels)	// Disable certain channels from triggering MIDI notes
{
	int i, ch;
	
	for(i = 0; i < 16; i++)
		canTriggerNoteOnChannel_[i] = true;
	for(i = 0; i < channels.size(); i++)
	{
		ch = channels[i];
		if(ch >= 0 || ch < 16)
			canTriggerNoteOnChannel_[ch] = false;
	}
}

// This gets called every time MIDI data becomes available on any input controller.  deltaTime gives us
// the time since the last event on the same controller, message holds a 3-byte MIDI message, and inputNumber
// tells us the number of the device that triggered it (see main.cpp for how this number is calculated).

// For now, we don't store separate state for separate devices: we use standard MIDI channels instead.
// channel 0 is the main (piano) keyboard, channel 1 the first auxiliary keyboard, and so on.

void MidiController::rtMidiCallback(double deltaTime, vector<unsigned char> *message, int inputNumber)
{
	if(message->size() == 0)	// Do nothing
		return;
    
    if (use_PA_)     PNOcontroller_->currentTimeStamp_ += deltaTime;
	
	// RtMidi will give us one MIDI command per callback, which makes processing easier for us.
#ifdef DEBUG_MESSAGES_RAW_MIDI
	cout << "~~~~~ MIDI " << inputNumber << ": ";
	for(int debugPrint = 0; debugPrint < message->size(); debugPrint++)
		printf("%x ", (*message)[debugPrint]);
	cout << endl;
#endif
	
	pthread_mutex_lock(&eventMutex_);	// Lock the event mutex: only one MIDI message at a time!
	
	unsigned char command = (*message)[0];
	
	if(command == MESSAGE_RESET)
	{
		// MIDI reset message, which we should respond to be clearing all input state
		cout << "MIDI Reset Received.  Clearing input state.\n";
		
		clearInputState();
	}
	else if(command < 0xF0)	// Commands below this need to be filtered for channel
	{
		set<Note*>::iterator it;
		unsigned int channel = command & 0x0F;
		command &= 0xF0;
		unsigned int pitchWheelValue;
		
		switch(command)		// In each case, make sure the message is long enough before proceeding (sanity check)
		{
			case MESSAGE_NOTEON:
				if(message->size() < 3 || !canTriggerNoteOnChannel_[channel])
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				// First, tell anyone else who might be listening to this note
				// Send all note on events to anyone who's listening, since wanting to know this kind of info is rare anyway.
				for(it = noteListeners_.begin(); it != noteListeners_.end(); it++)
				{
					(*it)->midiNoteEvent((*message)[0], (*message)[1], (*message)[2]);
				}
				// Some MIDI synths handle releases using Note On with velocity 0
				if((*message)[2] == 0)
					noteOff(deltaTime, message, inputNumber);
				else
				{
					noteOn(deltaTime, message, inputNumber);	// Create note
					checkForProgramUpdate((*message)[0] & 0x0F, (*message)[1]);			// See if this event updates the program number
				}
				break;
			case MESSAGE_NOTEOFF:
				if(message->size() < 3 || !canTriggerNoteOnChannel_[channel])
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				// First, tell anyone else who might be listening to this note
				// Send all note on events to anyone who's listening, since wanting to know this kind of info is rare anyway.
				for(it = noteListeners_.begin(); it != noteListeners_.end(); it++)
				{
					(*it)->midiNoteEvent((*message)[0], (*message)[1], (*message)[2]);
				}
				noteOff(deltaTime, message, inputNumber);
				break;
			case MESSAGE_AFTERTOUCH_POLY:
                //! If we're using the PNOScan, pass polyphonic aftertouch to the PNOscanController's method
                if (PNOcontroller_ != NULL && (*message)[2] > PNOSCAN_NOISE_THRESH && (*message)[1] >= 21) PNOcontroller_->handlePolyphonicAftertouch(message);
                
				if(message->size() < 3 || !canTriggerNoteOnChannel_[channel])
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				// Notify any notes that want to receive aftertouch
				for(it = aftertouchListeners_.begin(); it != aftertouchListeners_.end(); it++)
				{
					if((*it)->midiChannel() == channel && (*it)->midiNote() == (*message)[1])
						(*it)->midiAftertouch((*message)[2]);
				}                
				break;
			case MESSAGE_CONTROL_CHANGE:
				if(message->size() < 3)
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
#ifdef DEBUG_MESSAGES
				cout << "Control change: channel " << channel << ", control " << (int)(*message)[1] << " = " << (int)(*message)[2] << endl;
#endif
				// Check for global controllers
				if((*message)[1] == controlMasterVolume_)
				{
					float masterVolume;
					if((*message)[2] == 0)
						masterVolume = 0.0;
					else if((*message)[2] >= 64)
						masterVolume = pow(2.0, ((double)(*message)[2] - 64.0)/64.0);
					else
						masterVolume = pow(10.0, ((double)(*message)[2] - 64.0)/64.0);
					
					/*else
                     {
                     
                     masterVolume = pow(10.0, -(127.0-(float)(*message)[2])/64.0);	// About -40dB of attenuation max
                     }*/
					
					cout << "Master Volume: " << masterVolume << endl;
					render_->setGlobalAmplitude(masterVolume);
				}
				else if((*message)[1] == controlPitchTrackInputMute_)
				{
					if(pitchTrackController_ != NULL)
					{
						bool mute = ((*message)[2] < controlPitchTrackInputMuteThresh_);
						
						pitchTrackController_->setInputMute(mute);
						
						if(mute && !lastPitchTrackInputMute_)
						{
							cout << "Muting pitch-track input\n";
							lastPitchTrackInputMute_ = true;
						}
						else if(!mute && lastPitchTrackInputMute_)
						{
							cout << "Unmuting pitch-track input\n";
							lastPitchTrackInputMute_ = false;
						}
					}
				}
				
				if((*message)[1] == CONTROL_SOSTENUTO_PEDAL && INVERT_SOSTENUTO_PEDAL)
					(*message)[2] = 127 - (*message)[2];
				else if((*message)[1] == CONTROL_DAMPER_PEDAL && INVERT_DAMPER_PEDAL)
					(*message)[2] = 127 - (*message)[2];
				if((*message)[1] == CONTROL_ALL_NOTES_OFF || (*message)[1] == CONTROL_ALL_SOUND_OFF)
				{
					allNotesOff(channel);
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				if((*message)[1] == CONTROL_ALL_CONTROLLERS_OFF)
				{
					allControllersOff(channel);
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				if((*message)[1] == CONTROL_DAMPER_PEDAL && channel == 0)	 // Special treatment for damper pedal change on main piano
					damperPedalChange((*message)[2]);
				if((*message)[1] == CONTROL_SOSTENUTO_PEDAL && channel == 0) // Similarly, special handling for piano sost. pedal
					sostenutoPedalChange((*message)[2]);
				inputControllers_[channel][(*message)[1]] = (*message)[2];	// Store the new value (do this last)
                
				// Notify any notes that want to receive control changes
				it = controlListeners_.begin();
				
				//for(it = controlListeners_.begin(); it != controlListeners_.end(); it++)
				while(it != controlListeners_.end())
				{
					// We send all control change messages for any channel to the notes.  This isn't standard MIDI behavior
					// but the Note objects may want to know, for example, about the status of the pedals on the main keyboard.
					(*it++)->midiControlChange(channel, (*message)[1], (*message)[2]);
				}
				break;
			case MESSAGE_PROGRAM_CHANGE:
				if(message->size() < 2)
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
#ifdef DEBUG_MESSAGES
				cout << "Program change: channel " << channel << ", program " << (int)(*message)[1] << endl;
#endif
				//inputPatches_[channel] = (*message)[1];
				currentProgram_ = (*message)[1];
				oscSendPatchValue();
				if(pitchTrackController_ != NULL)
					pitchTrackController_->programChanged();
				break;
			case MESSAGE_AFTERTOUCH_CHANNEL:
				if(message->size() < 2 || !canTriggerNoteOnChannel_[channel])
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				// Notify any notes that want to receive aftertouch
				for(it = aftertouchListeners_.begin(); it != aftertouchListeners_.end(); it++)
				{
					if((*it)->midiChannel() == channel)
						(*it)->midiAftertouch((*message)[1]);
				}
				break;
			case MESSAGE_PITCHWHEEL:
				if(message->size() < 3)
				{
					pthread_mutex_unlock(&eventMutex_);
					return;
				}
				pitchWheelValue = (*message)[2] << 7 + (*message)[1];	// 14-bit value sent LSB first
				// Notify any notes that want to receive pitch wheel messages
				for(it = pitchWheelListeners_.begin(); it != pitchWheelListeners_.end(); it++)
				{
					if((*it)->midiChannel() == channel)
						(*it)->midiPitchWheelChange(pitchWheelValue);
				}
				break;
			default:		// Ignore (and this shouldn't happen anyway)
#ifdef DEBUG_MESSAGES
				cerr << "Warning: unexpected MIDI command " << command << endl;
#endif
				break;
		}
	}
	
	pthread_mutex_unlock(&eventMutex_);
}

// Set the MIDI note number of the first amplifier and the direction of the
// amplifiers (directionDown == true ---> the first amplifier is the highest string,
// else the first amplifier is the lowest string)

void MidiController::mrpSetBaseAndDirection(int firstString, bool directionDown, int offValue)
{
    mrpFirstString_ = firstString;
    mrpDirectionDown_ = directionDown;
    mrpOffValue_ = offValue;
}

// Specific function to communicate with MRP signal-routing hardware, telling it to connect a particular input to
// a given piano string

void MidiController::mrpSendRoutingMessage(int inputNumber, int stringNumber)
{
	vector<unsigned char> message;
	
	if(midiOut_ == NULL || inputNumber < 0 || inputNumber > 15)
		return;
	
	message.push_back(MESSAGE_CONTROL_CHANGE | (unsigned char)mrpChannel_);	// Use channel 0
	message.push_back(CONTROL_MRP_BASE + (unsigned char)inputNumber);
	message.push_back((unsigned char)(stringNumber & 0xFF));
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "mrpSendRoutingMessage(): input " << inputNumber << " string " << stringNumber << endl;
#endif
	
	try
	{
		midiOut_->sendMessage(&message);
	}
	catch(RtError& err)
	{
		RtError::Type errType = err.getType();
		
		if(errType == RtError::WARNING || errType == RtError::DEBUG_WARNING)
		{
			// Non-critical error, print but go on
			cerr << "MIDI output warning: ";
			err.printMessage();
		}
		else
		{
			cerr << "MIDI output error: ";
			err.printMessage();
		}
	}
}

void MidiController::mrpClearRoutingTable()
{
	vector<unsigned char> message;
	
	if(midiOut_ == NULL)
		return;
	
	message.push_back(MESSAGE_CONTROL_CHANGE | (unsigned char)mrpChannel_);	// Use channel 0
	message.push_back(CONTROL_ALL_CONTROLLERS_OFF);							// Tell it to reset all controllers
	message.push_back(0);													// Need this to satisfy MIDI spec
    
	try
	{
		midiOut_->sendMessage(&message);
	}
	catch(RtError& err)
	{
		RtError::Type errType = err.getType();
		
		if(errType == RtError::WARNING || errType == RtError::DEBUG_WARNING)
		{
			// Non-critical error, print but go on
			cerr << "MIDI output warning: ";
			err.printMessage();
		}
		else
		{
			cerr << "MIDI output error: ";
			err.printMessage();
		}
	}
}

// This method serves two purposes: first, it saves a reference to the OSC controller, which this class
// needs to pass it on to notes it creates.  Second, it registers this object to receive OSC messages
// (for example, patch changes).

void MidiController::setOscController(OscController *c)
{
	if(c == NULL)
		return;
    
	OscHandler::setOscController(c);
	
	string patchUpPath("/ui/patch/up"), patchDownPath("/ui/patch/down"), patchSetPath("/ui/patch/set");
	string allNotesOffStr("/ui/allnotesoff"), calibrationSave("/ui/cal/save"), calibrationLoad("/ui/cal/load");
	string damperPedalPath("/pedal/damper"), sostenutoPedalPath("/pedal/sostenuto");
    
    string globalVolume("/global/volume");
    string harmonicAmps("/global/harmonic_amplitudes");
    
    // To Do: attack, decay, modulation, mod frequency, holdoff velocity, holdoff position
	
	addOscListener(patchUpPath);
	addOscListener(patchDownPath);
	addOscListener(patchSetPath);
	addOscListener(allNotesOffStr);
	addOscListener(calibrationSave);
	addOscListener(calibrationLoad);
	addOscListener(damperPedalPath);
	addOscListener(sostenutoPedalPath);
    
    addOscListener(globalVolume);
    addOscListener(harmonicAmps);
}

// This method is called by the OscController when it receives a message we've registered for.  Returns true on success.

bool MidiController::oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data)
{
    cout << "MidiController::oscHandlerMethod()" << endl;
    
	float param;
	int pedalVal;
	set<Note*>::iterator it;
   
	if(!strcmp(path, "/pedal/damper") && numValues >= 1)	// Manually control the damper pedal
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			cout << "Damper pedal down (via OSC)\n";
			pedalVal = 127;
		}
		else
		{
			cout << "Damper pedal up (via OSC)\n";
			pedalVal = 0;
		}
		pthread_mutex_lock(&eventMutex_);
		damperPedalChange(pedalVal);
		inputControllers_[0][CONTROL_DAMPER_PEDAL] = pedalVal;
		
		// Notify any notes that want to receive control changes ( see rtMidiCallback() )
		it = controlListeners_.begin();
		while(it != controlListeners_.end())
			(*it++)->midiControlChange(0, CONTROL_DAMPER_PEDAL, pedalVal);
		pthread_mutex_unlock(&eventMutex_);
		return true;
	}
	if(!strcmp(path, "/pedal/sostenuto") && numValues >= 1)	// Manually control the damper pedal
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			cout << "Sostenuto pedal down (via OSC)\n";
			pedalVal = 127;
		}
		else
		{
			cout << "Sostenuto pedal up (via OSC)\n";
			pedalVal = 0;
		}
		pthread_mutex_lock(&eventMutex_);
		sostenutoPedalChange(pedalVal);
		inputControllers_[0][CONTROL_SOSTENUTO_PEDAL] = pedalVal;
		
		// Notify any notes that want to receive control changes ( see rtMidiCallback() )
		it = controlListeners_.begin();
		while(it != controlListeners_.end())
			(*it++)->midiControlChange(0, CONTROL_SOSTENUTO_PEDAL, pedalVal);
		pthread_mutex_unlock(&eventMutex_);
		return true;
	}
	if(!strcmp(path, "/ui/patch/up") && numValues >= 1)	// Increment the current program
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			consoleProgramIncrement();
			cout << "Program incremented to " << currentProgram_ << endl;
		}
		return true;
	}
	if(!strcmp(path, "/ui/patch/down") && numValues >= 1) // Decrement the current program
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			consoleProgramDecrement();
			cout << "Program decremented to " << currentProgram_ << endl;
		}
		return true;
	}
	if(!strcmp(path, "/ui/patch/set") && numValues >= 1) // Set the current program to the given parameter
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		consoleProgramChange((int)param);
		cout << "Program set to " << currentProgram_ << endl;
		return true;
	}
	if(!strcmp(path, "/ui/allnotesoff") && numValues >= 1) // Turn all current notes off
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			consoleAllNotesOff(-1);
			cout << "Sending 'All Notes Off'\n";
		}
		return true;
    }
	if(!strcmp(path, "/ui/cal/save") && numValues >= 1)
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			if(saveCalibrationTable(lastCalibrationFile_) == 0)
				cout << "Saved calibration values\n";
			else
				cout << "Error loading calibration values\n";
		}
		return true;
	}
	if(!strcmp(path, "/ui/cal/load") && numValues >= 1)
	{
		if(types[0] == 'i')
			param = values[0]->i;
		else if(types[0] == 'f')
			param = values[0]->f;
		else
			return false;
		
		if(param >= 0.5)
		{
			if(loadCalibrationTable(lastCalibrationFile_) == 0)
				cout << "Loaded calibration values\n";
			else
				cout << "Error loading calibration values\n";
		}
		return true;
	}
    
	return false;
}

bool MidiController::oscHandleGlobalParameters(const char *path, const char *types, int numValues, lo_arg **values, void *data)
{
    int harmonic = values[0]->i;
    float amplitude = values[1]->f;
    
    vector<double> targetHarmonicAmplitudes;
    vector<double> currentHarmonicAmplitudes;
    
    // Update the harmonic amplitudes for all currently sounding notes
    map<unsigned int, Note*>::iterator it;
    
    if(!currentNotes_.empty())
    {
        for(it = currentNotes_.begin(); it != currentNotes_.end(); ++it)
        {
            Note *note = (*it).second;

            RealTimeMidiNote *rtNote = (RealTimeMidiNote *)note;
            
            //! Get the current values of the harmonic amplitudes specified by the patch
            currentHarmonicAmplitudes = rtNote->getCurrentHarmonicAmplitudes();
            
            //! Set only the target harmonic
            targetHarmonicAmplitudes = currentHarmonicAmplitudes;
            targetHarmonicAmplitudes[harmonic-1] = amplitude;
            
#ifdef DEBUG_MESSAGES_EXTRA
            cout << "Setting harmonic amplitudes to ";
            for(int i = 0; i < currentHarmonicAmplitudes.size(); ++i)
            {                    
                cout << targetHarmonicAmplitudes[i] << " ";
            }
            cout << endl;
#endif
            rtNote->setRawHarmonicValues(targetHarmonicAmplitudes);
            rtNote->updateSynthParameters();
        }
    }

    cout << "============ Updating harmonic amplitudes for all prototype notes ==============" << endl;
    
    //! Now save the modified note as the new prototype note for all notes
    for(int midiNote = 0; midiNote < 128; ++midiNote)
    {
        for(int i = 0; i <= 3; ++i)
        {      
            Note *protoNote = programs_[PROGRAM_ID(currentProgram_, 0, midiNote)].notes[i];
            
            if (typeid(*protoNote) == typeid(RealTimeMidiNote))
            {
                RealTimeMidiNote *protoRtNote = (RealTimeMidiNote *)protoNote;
                
                protoRtNote->harmonic_->resetHarmonicAmplitudes(targetHarmonicAmplitudes);

//                protoRtNote->harmonic_->scaleHarmonicAmplitudes(targetHarmonicAmplitudes);
                
//                protoRtNote->setRawHarmonicValues(targetHarmonicAmplitudes);
                protoRtNote->updateSynthParameters();
            }
        }
    }
    return true;
}


// Send the current patch value out over OSC to the UI.  Returns true on success.

bool MidiController::oscSendPatchValue()
{
	if(oscController_ == NULL)
		return false;
	return oscController_->sendMessage("/ui/patch/number", "i", currentProgram_, LO_ARGS_END);
}

// FIXME: Should these listeners be mutex-protected?
void MidiController::addEventListener(Note *note, bool control, bool aftertouch, bool pitchWheel, bool otherNotes)
{
	// Add this Note object to the event listener sets for control changes, aftertouch, pitchwheel, and other note on/off events
	
	if(note == NULL)	// sanity check
		return;
	if(control)
		controlListeners_.insert(note);
	if(aftertouch)
		aftertouchListeners_.insert(note);
	if(pitchWheel)
		pitchWheelListeners_.insert(note);
	if(otherNotes)
		noteListeners_.insert(note);
}

void MidiController::removeEventListener(Note *note)
{
	// Remove this Note object from any listener sets.  If the note wasn't part of the sets, does nothing
	controlListeners_.erase(note);
	aftertouchListeners_.erase(note);
	pitchWheelListeners_.erase(note);
	noteListeners_.erase(note);
}

void MidiController::noteEnded(Note *note, unsigned int key)
{
	char oscMessageString[32];
	
	// Note objects call this when they finish releasing.  (Note that abort() might not call this.)
	// We should remove the indicated note from the map of current notes and return its audio channel to the pool.
	// Finally, delete the Note object.
    
#ifdef DEBUG_MESSAGES
	cout << "Note with key " << key << " ended.\n";
#endif
	
	removeEventListener(note);							// Cancel any notifications the note is waiting for
	render_->freeOutputChannel(note->audioChannel());	// Return output channel to the pool
	mrpSendRoutingMessage(note->mrpChannel(), 0);		// Disconnect the signal routing for this string
	
	if(currentNotes_.count(key) > 0)	// If the Note object has not removed itself during abort(), remove it from the map
		currentNotes_.erase(key);
	else
		cerr << "Warning: attempt to remove nonexistent key " << key << endl;
	
	// Finally, send an OSC message to the UI announcing the channel is free
	snprintf(oscMessageString, 32, "/ui/channel/note%d", note->mrpChannel());
	oscController_->sendMessage(oscMessageString, "s", "--", LO_ARGS_END);
	
	delete note;		// Will this crash??
}

bool MidiController::pianoDamperLifted(int note)
{
	// In case we run into a continuous pedal controller (Disklavier?), quarter-pedal is good enough for us.
	
	return ((inputControllers_[0][CONTROL_DAMPER_PEDAL] >= DAMPER_PEDAL_THRESHOLD) || (pianoDamperStates_[note] != DAMPER_DOWN));
}

// This static function performs a timed check for finished notes.  When it finds one, it tells the note to release.

void* MidiController::cleanupLoop(void *data)
{
	MidiController *controller = (MidiController *)data;
	
	while(!controller->cleanupShouldTerminate_)
	{
		/*map<unsigned int, Note*>::iterator it = controller->currentNotes_.begin();
         
         // Check if each note is finished, and release it if it is.
         
         while(it != controller->currentNotes_.end())
         {
         if((*it).second->isFinished())
         {
         #ifdef DEBUG_MESSAGES
         cout << "Found finished note " << (*it).second << ", aborting\n";
         #endif
         (*it++).second->abort();
         }
         else
         it++;
         }		*/
		// FIXME: This crashes!
		
		// Check every 10 ms.  Notes that are finished won't be actively rendering audio but they will be occupying a channel.
		// This is a reasonable compromise between responsiveness and overhead
		usleep(10000);
	}
	
	return NULL;
}

MidiController::~MidiController()
{    
	// Stop the cleanup thread
	cleanupShouldTerminate_ = true;
	pthread_join(cleanupThread_, NULL);
    
	patches_.clear();
	pthread_mutex_destroy(&eventMutex_);
}

#pragma mark -- Private Methods
// ** Private Methods **

void MidiController::noteOn(double deltaTime, vector<unsigned char> *message, int inputNumber)
{
	int midiChannel = ((*message)[0] & 0x0F);
	int midiNote = (*message)[1];
	int midiVelocity = (*message)[2];
	unsigned int key;
	int pianoString;
	char oscMessageString[32];
	Note *newNote;
	
    //! Update PNOscanController's Note map
    if (PNOcontroller_ != NULL)     PNOcontroller_->noteOn(message);
    
	// If this came from the piano keyboard, we should make a note that the key was pressed so that
	// we keep track of the state of the damper.
	
	if(midiChannel == 0)			// Main keyboard only
		pianoDamperStates_[midiNote] |= DAMPER_KEY;			// Damper is being held by key (possibly also by sost. ped.)
    
	
	// When a Note On message is received, we should instantiate a new Note object and tell it to begin
	// Add this note object to the map of currently sounding notes so we know what to release when we get
	// a Note Off message later.
    
	// First things first: let's check that this event actually corresponds to a note!
	
	if(programs_.count(PROGRAM_ID(currentProgram_, midiChannel, midiNote)) == 0)
	{
#ifdef DEBUG_MESSAGES_EXTRA
		cerr << "Warning: no Note found for program " << currentProgram_ << ", channel " << midiChannel << ", note " << midiNote << endl;
#endif
		return;
	}
	
	// Consult the program map to decide what kind of note to make
	
	bool damper = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].useDamperPedal;
	bool sostenuto = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].useSostenutoPedal;
	bool useAux = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].useAuxPedal;
    bool sustainAlways = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].sustainAlways;
	bool auxActive = (inputControllers_[0][CONTROL_AUX_PEDAL] >= 64);
	int velocitySplit = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].velocitySplitPoint;
	int priority = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].priority;
    float thisNoteAmplitudeOffset = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].amplitudeOffset;
	int noteIndex = 0;
	
	key = ((unsigned int)midiChannel << 8) + (unsigned int)midiNote;
    cout << "NoteMapKey = " << key << endl;
	
	// Make sure there isn't already a note playing with this key
	if(currentNotes_.count(key) > 0)
	{
		cerr << "Warning: duplicate note " << midiNote << " (channel " << midiChannel << ")\n";
		
		Note *duplicateNote = currentNotes_[key];
		duplicateNote->abort();
		removeEventListener(duplicateNote);							// Remove the note from any event listeners
		render_->freeOutputChannel(duplicateNote->audioChannel());	// Return its output channel to the pool
	}
    
    // If this note is assigned to a monophonic voice, check if there is any other note
    // present in the voice, and if so, turn it off.
    int monoVoice = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].monoVoice;
	if(monoVoice >= 0 && monoVoice < 16)
    {
        int previousKeyInVoice = monoVoiceNotes_[monoVoice];
        
        if(previousKeyInVoice >= 0 && currentNotes_.count(previousKeyInVoice) > 0) {
            cout << "Deactiving note with key " << previousKeyInVoice << " in mono voice " << monoVoice << endl;
            
            Note *previousNoteInVoice = currentNotes_[previousKeyInVoice];
            previousNoteInVoice->abort();
            removeEventListener(previousNoteInVoice);
            render_->freeOutputChannel(previousNoteInVoice->audioChannel());
        }
        
        monoVoiceNotes_[monoVoice] = key;
    }
	
	// Now, allocate an audio channel for this note.  But only if we have the space, or if there are lower-priority notes
	// to turn off.
	
	pair<int,int> channels = render_->allocateOutputChannel();
	if(channels.first == -1)											// No channel available, or error occurred
	{
		// Tell the oldest note in the currently sounding collection to turn off
		map<unsigned int, Note*>::iterator it;
		double oldestTime = (double)render_->currentTime();	// Other notes should be older than this
		int lowestPriority = INT_MAX;
		Note *oldestNote = NULL;
        
		// First, look for the lowest priority among current notes
		for(it = currentNotes_.begin(); it != currentNotes_.end(); it++)
		{
			Note *n = it->second;
			if(n->priority() < lowestPriority)
				lowestPriority = n->priority();
		}
		
		// Compare the lowest priority in the current collection to the priority of the note
		// that we propose to turn on.
		if(lowestPriority > priority || (lowestPriority == priority && !displaceOldNotes_))
		{
			cerr << "No channel available for note " << midiNote << endl;
			return;
		}
		
		// Now, break ties by finding the oldest note with the lowest priority
		for(it = currentNotes_.begin(); it != currentNotes_.end(); it++)
		{
			Note *n = it->second;
			if(n->priority() > lowestPriority)
				continue;
			if(n->startTime() < oldestTime)
			{
				oldestTime = n->startTime();
				oldestNote = n;
			}
		}
		if(oldestNote != NULL)
		{
			// Found the oldest note.  Turn it off.
			cout << "Out of channels: turning off note with key " << oldestNote->midiNote() << endl;
			oldestNote->abort();
		}
		
		// Now try again...
		channels = render_->allocateOutputChannel();
		if(channels.first == -1)
		{
			cerr << "No channel available for note " << midiNote << endl;
			return;
		}
	}
    
	// Send a MIDI output message (if we're using MIDI out) to the router hardware,
	// sending this note to the correct string
    
	pianoString = stringNoteMaps_[midiNote];
    
    // Transform the string number to match the setup of the MRP controller
    if(mrpDirectionDown_ && mrpFirstString_ - pianoString >= 0)
        mrpSendRoutingMessage(channels.second, mrpFirstString_ - pianoString);
    else if(pianoString - mrpFirstString_ >= 0)
        mrpSendRoutingMessage(channels.second, pianoString - mrpFirstString_);
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "Phase offset = " << phaseOffsets_[midiNote] << ", amplitude offset = " << amplitudeOffsets_[midiNote] << endl;
#endif
	
	// aux and velocitySplit will tell us which of the patches from this program we should use
	if(useAux && auxActive)		// Aux pedal is depressed, and we're listening to it
	{
		if(midiVelocity >= velocitySplit && velocitySplit > 0)
			noteIndex = 3;	// Aux, high velocity
		else
			noteIndex = 2;	// Aux, low velocity
	}
	else
	{
		if(midiVelocity >= velocitySplit && velocitySplit > 0)
			noteIndex = 1;	// Main, high velocity
		else
			noteIndex = 0;	// Main, low velocity
	}
	
	Note *oldNote = programs_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)].notes[noteIndex];
	
	cout << "currentProgram_ " << currentProgram_ << " channel " << midiChannel << " noteIndex " << noteIndex << " Note " << oldNote << endl;
	if(oldNote == NULL)
	{
		cerr << "No Note for program " << currentProgram_ << ", channel " << midiChannel << ", note " << midiNote;
		cerr << ", noteIndex " << noteIndex << endl;
		render_->freeOutputChannel(channels.first);
		return;
	}
	
	// TODO: Velocity map in here-- use lookup table with midiVelocity as index
	
	if(typeid(*oldNote) == typeid(MidiNote))					// Check class type
	{
		// The note in the program table holds factory classes which will instantiate the correct synths for this instance
		newNote = ((MidiNote*)oldNote)->createNote(channels.first, channels.second, midiNote, midiChannel, stringNoteMaps_[midiNote], key,
												   priority, midiVelocity, phaseOffsets_[midiNote], amplitudeOffsets_[midiNote]);
		newNote->setResponseToPedals(damper, sostenuto);
	}
	else if(typeid(*oldNote) == typeid(RealTimeMidiNote))					// Check class type
	{
		// The note in the program table holds factory classes which will instantiate the correct synths for this instance
		newNote = ((RealTimeMidiNote*)oldNote)->createNote(channels.first, channels.second, midiNote, midiChannel, stringNoteMaps_[midiNote], key,
                                                           priority, midiVelocity, phaseOffsets_[midiNote], amplitudeOffsets_[midiNote],
														   0.0, 0.0, 0.0, 0.0);
		newNote->setResponseToPedals(damper, sostenuto);
	}
	else if(typeid(*oldNote) == typeid(CalibratorNote))
	{
		newNote = ((CalibratorNote*)oldNote)->createNote(channels.first, channels.second, midiNote, midiChannel, stringNoteMaps_[midiNote], key,
														 priority, midiVelocity, phaseOffsets_[midiNote], amplitudeOffsets_[midiNote]);
		newNote->setResponseToPedals(damper, sostenuto);
		((CalibratorNote *)newNote)->setOscController(oscController_);	// This type responds to OSC messages
	}
	else if(typeid(*oldNote) == typeid(ResonanceNote))
	{
		newNote = ((ResonanceNote*)oldNote)->createNote(channels.first, channels.second, midiNote, midiChannel, stringNoteMaps_[midiNote], key,
                                                        priority, midiVelocity, phaseOffsets_[midiNote], amplitudeOffsets_[midiNote]);
		newNote->setResponseToPedals(damper, sostenuto);
	}
	else
	{
		cerr << "Unknown Note type for program " << currentProgram_ << ", channel " << midiChannel << ", note " << midiNote << endl;
		render_->freeOutputChannel(channels.first);
		return;
	}
    
	// Add the note to the map of currently playing notes.  The key is an int containing both the channel
	// and the note number, to ensure that each key is unique.
	
#ifdef DEBUG_MESSAGES
	cout << "Note on: " << midiNote << " (vel " << midiVelocity << ")" << " output " << channels.first << "/" << channels.second << " (string ";
	cout << (int)stringNoteMaps_[midiNote] << ") key = " << key << endl;
#endif
	
	newNote->begin(pianoDamperLifted(pianoString));			// Tell note to begin, and let it know whether damper is up
	currentNotes_[key] = newNote;							// Store this note object in the map
	
	if(midiNote >= 21 && midiNote <= 108)
	{
		// Finally, send an OSC message to the UI announcing the note # and channel
		snprintf(oscMessageString, 32, "/ui/channel/note%d", channels.first);
		oscController_->sendMessage(oscMessageString, "s", kNoteNames[midiNote-21], LO_ARGS_END);
	}
}

// For a given MIDI note, check if this triggers a program change.  This is called separately from noteOn()
// so that the PianoBar controller can be more selective in when it triggers this event.

void MidiController::checkForProgramUpdate(int midiChannel, int midiNote)
{
	if(programTriggeredChanges_.count(PROGRAM_ID(currentProgram_, midiChannel, midiNote)) != 0)
	{
		int newProgram = programTriggeredChanges_[PROGRAM_ID(currentProgram_, midiChannel, midiNote)];
		cout << "Changing Program to " << newProgram << endl;
		currentProgram_ = newProgram;
	}
}

void MidiController::noteOff(double deltaTime, vector<unsigned char> *message, int inputNumber)
{
	int midiChannel = ((*message)[0] & 0x0F);
	int midiNote = (*message)[1];
	int midiVelocity = (*message)[2];			// Don't use note off velocity in this program
	
#ifdef DEBUG_MESSAGES
	cout << "Note off: " << midiNote << " (vel " << midiVelocity << ")\n";
#endif
	
	if(midiChannel == 0)
	{
		// Check whether this noteOff message means that a piano damper went down.
		pianoDamperStates_[midiNote] &= ~DAMPER_KEY;
	}
	
	// See if there's a note in the map that matches this Note Off message, and if so, tell it to release
	
	unsigned int key = ((unsigned int)midiChannel << 8) + (unsigned int)midiNote;
	if(currentNotes_.count(key) > 0)
	{
		Note *note = currentNotes_[key];
		
		if(pianoDamperLifted(note->pianoString()))		// Damper is still up, because of either damper or sost. pedal
		{
#ifdef DEBUG_MESSAGES
			cout << "Note " << midiNote << " released, but damper still up\n";
#endif
			note->release();				// Don't do anything else with it for now; we'll handle it when the damper goes up
		}
		else
		{
#ifdef DEBUG_MESSAGES
			cout << "Damper is/went down on string " << midiNote << endl;
#endif
			note->releaseDamperDown();
		}
		// When the note actually terminates (as opposed to just beginning to release), we have to remove it
		// from the map, but we don't do that here.
	}
	else
	{
		// Do nothing, since there doesn't seem to be any note like this!
		// It's possible that our audio channels have filled up, and that's why there's no note
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Warning: no note found for NoteOff message, note = " << (int)(*message)[1] << " (channel " << (int)((*message)[0] & 0x0F) << ")\n";
#endif
	}
    
    //! Update PNOscanController's Note map
    if (PNOcontroller_ != NULL)     PNOcontroller_->noteOff(message);
}

void MidiController::damperPedalChange(unsigned char value)
{
	// If the damper pedal was depressed and has been released, we need to alert any notes that are using strings whose
	// dampers will be coming down.
	
	if(inputControllers_[0][CONTROL_DAMPER_PEDAL] < DAMPER_PEDAL_THRESHOLD || value >= DAMPER_PEDAL_THRESHOLD)
		return;	// No need to do anything if either the pedal was previous not depressed, or if it's still depressed
	
	// Iterate through the current notes (usually max 16, so not that slow) to identify any that need to be alerted
	map<unsigned int, Note*>::iterator it = currentNotes_.begin();
	
	//for(it = currentNotes_.begin(); it != currentNotes_.end(); it++)
	while(it != currentNotes_.end())
	{
		int stringUsed = ((*it).second)->pianoString();
		
		if(stringUsed < 0 || stringUsed > 128)	// Sanity check
		{
			cerr << "Warning: Invalid string in damperPedalChange\n";
			continue;
		}
		if(pianoDamperStates_[stringUsed] == DAMPER_DOWN)	// Damper has not been held up any other way
		{
#ifdef DEBUG_MESSAGES
			cout << "Damper went down on string " << stringUsed << ", output " << ((*it).second)->audioChannel() << endl;
#endif
			((*it++).second)->releaseDamperDown();
		}
		else
			++it;
	}
}

void MidiController::sostenutoPedalChange(unsigned char value)
{
	int i;
	
	// If the sostenuto pedal is depressed when it previously has not been, any strings whose dampers are currently up should
	// be changed in state to reflect the fact that they'll be held up by the sost. pedal after the key is released.
	
	// On the other hand, if the sostenuto pedal is released when it was depressed, any strings in the sost. pedal state should
	// be updated.  Notes that use these strings may need to be alerted.
	
	if(inputControllers_[0][CONTROL_SOSTENUTO_PEDAL] < SOSTENUTO_PEDAL_THRESHOLD && value >= SOSTENUTO_PEDAL_THRESHOLD)
	{
		for(i = 0; i < 128; i++)						// Pedal is now depressed-- figure out what dampers are up
		{
			if(pianoDamperStates_[i] & DAMPER_KEY)
			{
#ifdef DEBUG_MESSAGES
				cout << "Note " << i << " held in sostenuto pedal.\n";
#endif
				pianoDamperStates_[i] |= DAMPER_SOSTENUTO;
			}
		}
	}
	else if(inputControllers_[0][CONTROL_SOSTENUTO_PEDAL] >= SOSTENUTO_PEDAL_THRESHOLD && value < SOSTENUTO_PEDAL_THRESHOLD)
	{
		if(inputControllers_[0][CONTROL_DAMPER_PEDAL] < DAMPER_PEDAL_THRESHOLD)	// Damper pedal not depressed
		{
			// Iterate through the current notes (usually max 16, so not that slow) to identify any that need to be alerted
			map<unsigned int, Note*>::iterator it = currentNotes_.begin();
			
			//for(it = currentNotes_.begin(); it != currentNotes_.end(); it++)
			while(it != currentNotes_.end())
			{
				int stringUsed = ((*it).second)->pianoString();
                
				if(stringUsed < 0 || stringUsed > 128)	// Sanity check
				{
					cerr << "Warning: Invalid string in sostenutoPedalChange\n";
					continue;
				}
                
				if(pianoDamperStates_[stringUsed] == DAMPER_SOSTENUTO)	// Was being held by sost. pedal, but not by key
				{
#ifdef DEBUG_MESSAGES
					cout << "Note " << ((*it).second)->midiNote() << " released on sost. pedal release\n";
#endif
					((*it++).second)->releaseDamperDown();	// Increment the iterator before the release happens
				}
				else
					++it;
			}
		}
		for(i = 0; i < 128; i++)						// Update string states to reflect the dampers coming down
		{
#ifdef DEBUG_MESSAGES
			if(pianoDamperStates_[i] == DAMPER_SOSTENUTO)
				cout << "Damper went down on string " << i << endl;
#endif
			pianoDamperStates_[i] &= ~DAMPER_SOSTENUTO;
		}
	}
}

vector<int> MidiController::parseRangeString(const string& inString)
{
	vector<int> out;
	char *str, *strOrig, *ap, *ap2;
	int num, range[2], index;
    
	str = strOrig = strdup(inString.c_str());
	
	// Parse a string of the format "1,2-4,3-10,11,12" containing individual numbers and ranges of numbers.  The resulting
	// vector will hold all the integers included in this range.
	
	// First, parse out values separated by commas
	while((ap = strsep(&str, ",; \t")) != NULL)
	{
		if((*ap) == '\0')	// Empty field
			continue;
		
		if(strchr(ap, '-') != NULL)
		{
			index = 0;
			
			// Next, this string might itself be a range of numbers (e.g. "5-10") so we should separate by dashes
			while((ap2 = strsep(&ap, "-")) != NULL)
			{
				if((*ap2) == '\0')		// Ignore blank tokens
					continue;
				num = (int)strtol(ap2, NULL, 10);
                
				range[index++] = num;
				if(index >= 2)							// Only take the first two numbers
					break;
			}
			if(index == 0)								// No valid tokens found
			{
				cerr << "Warning: invalid range string '" << ap2 << "' in parseRangeString()\n";
			}
			else if(index == 1)							// Found one number only
			{
				cerr << "Warning: range string '" << ap2 << "' contains only one token in parseRangeString()\n";
				out.push_back(range[0]);
			}
			else										// Have a range of two numbers
			{
				int begin = (range[0] <= range[1] ? range[0] : range[1]);	// Put them in ascending order
				int end = (range[0] <= range[1] ? range[1] : range[0]);
				
				for(int i = begin; i <= end; i++)							// Add the complete range (inclusive)
					out.push_back(i);
			}
		}
		else
		{
			// This entry is taken to be a single number
			num = (int)strtol(ap, NULL, 10);
            
			out.push_back(num);
		}
	}
	
	free(strOrig);	// This pointer won't move like str
	return out;
}