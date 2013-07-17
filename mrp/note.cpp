/*
 *  note.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/29/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include <cmath>
#include "note.h"
#include "config.h"

const char *kNoteNames[88] = { "A0", "A#0", "B0",
	"C1", "C#1", "D1", "D#1", "E1", "F1", "F#1", "G1", "G#1", "A1", "A#1", "B1",
	"C2", "C#2", "D2", "D#2", "E2", "F2", "F#2", "G2", "G#2", "A2", "A#2", "B2",
	"C3", "C#3", "D3", "D#3", "E3", "F3", "F#3", "G3", "G#3", "A3", "A#3", "B3",
	"C4", "C#4", "D4", "D#4", "E4", "F4", "F#4", "G4", "G#4", "A4", "A#4", "B4",
	"C5", "C#5", "D5", "D#5", "E5", "F5", "F#5", "G5", "G#5", "A5", "A#5", "B5",
	"C6", "C#6", "D6", "D#6", "E6", "F6", "F#6", "G6", "G#6", "A6", "A#6", "B6",
	"C7", "C#7", "D7", "D#7", "E7", "F7", "F#7", "G7", "G#7", "A7", "A#7", "B7", "C8"};	

#pragma mark MidiNote

/*MidiNote::MidiNote(const MidiNote& copy) : Note(copy), velocity_(copy.velocity_), name_(copy.name_)
{
	// Copy a new set of Synths to this note.
	for(int i = 0; i < copy.synths_.size(); i++)
	{
		if(typeid(*copy.synths_[i]) == typeid(PllSynth))
		{
			PllSynth *s = new PllSynth(*dynamic_cast<PllSynth*>(copy.synths_[i]));
			synths_.push_back(s);
		}
		else if(typeid(*copy.synths_[i]) == typeid(NoiseSynth))
		{
			NoiseSynth *s = new NoiseSynth(*dynamic_cast<NoiseSynth*>(copy.synths_[i]));
			synths_.push_back(s);
		}
		else
		{
			cerr << "Warning: unknown synth type in MidiNote copy constructor\n";
		}
	}
	// FIXME: Copy factories
}

MidiNote& MidiNote::operator=(const MidiNote& copy)
{
	if(this == &copy)
		return *this;
	
	for(int i = 0; i < synths_.size(); i++)
		delete synths_[i];
	synths_.clear();
	
	Note::operator=(copy);
	
	// FIXME: Copy factories
	velocity_ = copy.velocity_;
	name_ = copy.name_;
	
	// Copy a new set of Synths to this note.
	for(int i = 0; i < copy.synths_.size(); i++)
	{
		if(typeid(*copy.synths_[i]) == typeid(PllSynth))
		{
			PllSynth *s = new PllSynth(*dynamic_cast<PllSynth*>(copy.synths_[i]));
			synths_.push_back(s);
		}
		else if(typeid(*copy.synths_[i]) == typeid(NoiseSynth))
		{
			NoiseSynth *s = new NoiseSynth(*dynamic_cast<NoiseSynth*>(copy.synths_[i]));
			synths_.push_back(s);
		}
		else
		{
			cerr << "Warning: unknown synth type in MidiNote copy constructor\n";
		}
	}	
	
	return *this;
}*/

// Parse an XML data structure to get all the parameters we need.  This will generally be called
// when the patch table is first loaded, to make one reference instance of each patch.  These
// reference instances will be copied every time a new note is triggered, and the performance-specific
// data (MIDI note #, velocity, output channel, etc.) will be passed in at that time.

int MidiNote::parseXml(TiXmlElement *baseElement)
{
	// Presently, we look for Synth tags within this element.  A note can have one or more synths, each of which
	// has a set of ramping parameters.
	int numSynths = 0;
	TiXmlElement *element;
	const string *name = baseElement->Attribute((const string)"name");
	
	if(name != NULL)
		name_ = *name;
	
	synths_.clear();				// Clear out old data
	factories_.clear();
	
	// Check for a <VelocityCurve> tag to tell us how to shape the incoming velocity parameter.
	// If none, use the default 0.0
	
	element = baseElement->FirstChildElement("VelocityCurve");
	if(element != NULL)
	{
		TiXmlHandle elementHandle(element);
		TiXmlText *text;
		
		text = elementHandle.FirstChild().ToText();
		if(text != NULL)
		{
			stringstream s(text->ValueStr());
			s >> velocityCurve_;
			
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "Velocity curve set to " << velocityCurve_ << endl;
#endif
		}
	}

	// Allow "a/b" ranges on values to specify min and max velocity; if only one is given, a=b
	// Use a "concavity" attribute to specify concavity (default is 0)
	// Eventually, allow a velocity pre-mapping

	element = baseElement->FirstChildElement("Synth");
	
	while(element != NULL)
	{
		const string *synthClass = element->Attribute((const string)"class");	// Attribute tells us the kind of synth
		
		if(synthClass == NULL)		// If we can't find a class attribute, skip it
		{
			cerr << "MidiNote::parseXml() warning: Synth object with no class\n";
			element = element->NextSiblingElement("Synth");
			continue;
		}
		if(synthClass->compare("PllSynth") == 0 ) // Two types of synths available
		{
			// Each synth holds a collection of parameters
			TiXmlElement *element2 = element->FirstChildElement("Parameter");
			PllSynthFactory *pllSynthFactory = new PllSynthFactory(controller_);

			pllSynthFactory->sampleRate_ = render_->sampleRate();

			while(element2 != NULL)
			{
				assignPllSynthParameters(pllSynthFactory, element2);
				element2 = element2->NextSiblingElement("Parameter");
			}

			factories_.push_back(pllSynthFactory);
		}
		else if(synthClass->compare("NoiseSynth") == 0)
		{
			// Each synth holds a collection of parameters
			TiXmlElement *element2 = element->FirstChildElement("Parameter");
			NoiseSynthFactory *noiseSynthFactory = new NoiseSynthFactory(controller_);
			
			noiseSynthFactory->sampleRate_ = render_->sampleRate();
			
			while(element2 != NULL)
			{
				assignNoiseSynthParameters(noiseSynthFactory, element2);
				element2 = element2->NextSiblingElement("Parameter");
			}
			
			factories_.push_back(noiseSynthFactory);			
		}
		else
		{
			cerr << "MidiNote::parseXml() warning: Unknown Synth class '" << *synthClass << "'\n";
		}
		
		numSynths++;
		element = element->NextSiblingElement("Synth");
	}
	
	if(numSynths == 0)
		cerr << "MidiNote::parseXml() warning: no Synths found\n";
	
	return 0;
}

// Is this even important anymore?
void MidiNote::setPerformanceParameters(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, 
										unsigned int key, int priority, int velocity)
{
	Note::setPerformanceParameters(audioChannel, mrpChannel, midiNote, midiChannel, pianoString, key, priority);
	
	velocity_ = velocity;
}

// Tells this note whether to continue sustaining if prolonged by either damper or sostenuto pedals.
// Check this information at release time to decide whether to call abort() or not.
void MidiNote::setResponseToPedals(bool damper, bool sostenuto)
{
	sustainOnDamperPedal_ = damper;
	sustainOnSostenutoPedal_ = sostenuto;
}

void MidiNote::begin(bool damperLifted)
{
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "begin()\n";
#endif
	if(isRunning_)
		return;
	
	// Insert each synth into the render list, and tell it to begin
	for(int i = 0; i < synths_.size(); i++)
	{
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "adding Synth " << synths_[i] << endl;
#endif
		if(render_->addSynth(synths_[i]))
			cerr << "MidiNote::begin() warning: error adding synth #" << i << endl;
		synths_[i]->begin();
	}
	
	isRunning_ = true;
}

void MidiNote::release()
{
	isReleasing_ = true;
	
	if(!sustainOnDamperPedal_ && !sustainOnSostenutoPedal_)		// If we don't respond to either pedal, finish up now
	{
		abort();
		return;
	}
	else
	{
		unsigned char damper = controller_->getControllerValue(0, MidiController::CONTROL_DAMPER_PEDAL);
		unsigned char sost = controller_->getControllerValue(0, MidiController::CONTROL_SOSTENUTO_PEDAL);
		
		if(!((sustainOnDamperPedal_ && damper >= DAMPER_PEDAL_THRESHOLD)			// If neither pedal is sustaining us...
			 /*|| (sustainOnSostenutoPedal_ && sost >= SOSTENUTO_PEDAL_THRESHOLD)))*/
			 || (sustainOnSostenutoPedal_ && controller_->pianoDamperState(midiNote_) & MidiController::DAMPER_SOSTENUTO)))
		{
			abort();															// ...then finish up.
			return;
		}
	}

	// If this hasn't called abort(), we'll get MIDI control change messages until eventually we do abort.
	controller_->addEventListener(this, true, false, false, false);
}

void MidiNote::abort()
{
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "abort()\n";
#endif
	
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
			cerr << "MidiNote::abort() warning: error removing synth #" << i << endl;
	}
	
	isRunning_ = false;	
	controller_->noteEnded(this, key_);			// Tell the controller we finished
}

bool MidiNote::isFinished()
{
	bool finished = true;
	
	// Check whether each synth is finished; if so, return true

	for(int i = 0; i < synths_.size(); i++)
	{
		if(!synths_[i]->isFinished())
			finished = false;
	}
	
	return finished;
}

void MidiNote::midiControlChange(unsigned char channel, unsigned char control, unsigned char value)
{
	// Check for damper or sostenuto pedal changes, if we're in release mode.  Stop the note if the right pedal change occurs
	
	if(!isReleasing_)	// Sanity check-- should only get here during release anyway
		return;
	if(channel != 0)	// Don't care about controls that aren't from the main keyboard
		return;
	if(control == MidiController::CONTROL_DAMPER_PEDAL && sustainOnDamperPedal_)
	{
		// Check if pedal has been released; if so, check whether sostenuto pedal is still holding this note.
		if(value >= DAMPER_PEDAL_THRESHOLD)
			return;
		if(sustainOnSostenutoPedal_ && controller_->getControllerValue(0, MidiController::CONTROL_SOSTENUTO_PEDAL) >= SOSTENUTO_PEDAL_THRESHOLD)
			return;
		
		abort(); // Otherwise, the damper pedal is now released and the note should end
		return;
	}
	else if(control == MidiController::CONTROL_SOSTENUTO_PEDAL && sustainOnSostenutoPedal_)
	{
		// Check if pedal has been released
		if(value >= SOSTENUTO_PEDAL_THRESHOLD)
			return;
		// Given that sostenuto pedal has been released, this note should end even if the pedal comes back down later.
		// On the other hand, keep sustaining it if it also responds to the damper pedal
		sustainOnSostenutoPedal_ = false;
		if(sustainOnDamperPedal_ && controller_->getControllerValue(0, MidiController::CONTROL_DAMPER_PEDAL) >= DAMPER_PEDAL_THRESHOLD)
			return;
		
		abort();
		return;
	}
}

// This method creates and returns a copy of the object, but instead of containing factories, it contains real
// Synth objects with the right parameters for the particular MIDI note and velocity.

MidiNote* MidiNote::createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key, 
							   int priority, int velocity, float phaseOffset, float amplitudeOffset)
{
	MidiNote *out = new MidiNote(controller_, render_);
	int i;
	float baseFreq;
	
	out->setPerformanceParameters(audioChannel, mrpChannel, midiNote, midiChannel, pianoString, key, priority, velocity);
	
	baseFreq = controller_->midiNoteToFrequency(midiNote);				// Find the center frequency for this note
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "createNote(): baseFreq = " << baseFreq << endl;
#endif
	
	// Using the factory classes, create synths that reflect the specific parameters of this note
	
	for(i = 0; i < factories_.size(); i++)
	{
		SynthBase *synth = factories_[i]->createSynth(midiNote, velocity, velocityCurve_, amplitudeOffset);
		timedParameter emptyParam;

		if(typeid(*synth) == typeid(PllSynth))
			((PllSynth*)synth)->setPhaseOffset(phaseOffset, emptyParam);
		synth->setPerformanceParameters(render_->numInputChannels(), render_->numOutputChannels(), audioChannel);
		out->synths_.push_back(synth);
	}	
	
	return out;
}

MidiNote::~MidiNote()
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "**** ~MidiNote\n";
#endif
	
	for(i = 0; i < synths_.size(); i++)
		delete synths_[i];
	for(i = 0; i < factories_.size(); i++)
		delete factories_[i];
}

#pragma mark Private Methods

// Private utility method that assigns parameter values to a synth based on XML data

void MidiNote::assignPllSynthParameters(PllSynthFactory *factory, TiXmlElement *element)
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
		int i;
		double c = 0.0;
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "assignPllSynthParameters(): Processing name = '" << (*name) << "', value = '" << (*value) << "'\n";
#endif
		if(concavity != NULL)		// Read the concavity attribute if present; if not, assume linear (0)
		{
			stringstream concavityStream(*concavity);
			concavityStream >> c;
		}
		
		// First, check that the name is a parameter we recognize.  Some will have float values, others will be a list of floats.
		// We don't look for a phase offset parameter in the XML file, since that is separately calibrated
		if(name->compare("FilterQ") == 0) {							// double, time-invariant
			valueStream >> factory->filterQ_;
			factory->filterQActive_ = true;
		}
		else if(name->compare("LoopFilterPole") == 0) {				// double, time-invariant
			valueStream >> factory->loopFilterPole_;
			factory->loopFilterPoleActive_ = true;
		}
		else if(name->compare("LoopFilterZero") == 0) {				// double, time-invariant
			valueStream >> factory->loopFilterZero_;
			factory->loopFilterZeroActive_ = true;
		}
		else if(name->compare("UseAmplitudeFeedback") == 0) {		// bool, time-invariant
			valueStream >> boolalpha >> factory->useAmplitudeFeedback_;
			factory->useAmplitudeFeedbackActive_ = true;
		}
		else if(name->compare("UseInterferenceRejection") == 0) {	// bool, time-invariant
			valueStream >> boolalpha >> factory->useInterferenceRejection_;
			factory->useInterferenceRejectionActive_ = true;	
		}
		else if(name->compare("RelativeFrequency") == 0) {			// double, time-variant
			parseVelocityPair(value->c_str(), &(factory->relativeFrequencyMin_.start), &(factory->relativeFrequencyMax_.start));
			parseParameterRampWithVelocity(element, &(factory->relativeFrequencyMin_.ramp), &(factory->relativeFrequencyMax_.ramp));
			factory->relativeFrequencyConcavity_ = c;
			factory->relativeFrequencyActive_ = true;
			/*timedParameter tp;
			valueStream >> d;			
			if(!foundRelativeFrequency_)	// Check if we've already seen this tag
			{
				tp = parseParameterRamp(element);
				startRelativeFrequencies_.push_back(d);
				rampRelativeFrequencies_.push_back(tp);
				foundRelativeFrequency_ = true;
			}*/
		}
		else if(name->compare("GlobalAmplitude") == 0) {			// double, time-variant
			parseVelocityPair(value->c_str(), &(factory->globalAmplitudeMin_.start), &(factory->globalAmplitudeMax_.start));
			parseParameterRampWithVelocity(element, &(factory->globalAmplitudeMin_.ramp), &(factory->globalAmplitudeMax_.ramp));
			factory->globalAmplitudeConcavity_ = c;
			factory->globalAmplitudeActive_ = true;			
			/*timedParameter tp;
			valueStream >> d;			
			if(!foundRelativeAmplitude_)	// Check if we've already seen this tag
			{
				tp = parseParameterRamp(element);
				startRelativeAmplitudes_.push_back(d);
				rampRelativeAmplitudes_.push_back(tp);
				foundRelativeAmplitude_ = true;
			}*/
		}
		else if(name->compare("HarmonicAmplitudes") == 0) {			// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
	
			factory->harmonicAmplitudesMin_.clear();					// Form them into paramHolder structs
			factory->harmonicAmplitudesMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->harmonicAmplitudesMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->harmonicAmplitudesMax_.push_back(ph2);
			}
			factory->harmonicAmplitudesConcavity_ = c;
			factory->harmonicAmplitudesActive_ = true;
		}
		else if(name->compare("HarmonicPhases") == 0) {				// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
			
			factory->harmonicPhasesMin_.clear();					// Form them into paramHolder structs
			factory->harmonicPhasesMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->harmonicPhasesMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->harmonicPhasesMax_.push_back(ph2);
			}
			factory->harmonicPhasesConcavity_ = c;
			factory->harmonicPhasesActive_ = true;	
		}
		else if(name->compare("InputGains") == 0) {					// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
			
			factory->inputGainsMin_.clear();					// Form them into paramHolder structs
			factory->inputGainsMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->inputGainsMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->inputGainsMax_.push_back(ph2);
			}
			factory->inputGainsConcavity_ = c;
			factory->inputGainsActive_ = true;
		}
		else if(name->compare("InputDelays") == 0) {				// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
			
			factory->inputDelaysMin_.clear();					// Form them into paramHolder structs
			factory->inputDelaysMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->inputDelaysMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->inputDelaysMax_.push_back(ph2);
			}
			factory->inputDelaysConcavity_ = c;
			factory->inputDelaysActive_ = true;
		}
		else if(name->compare("LoopGain") == 0) {					// double, time-variant
			parseVelocityPair(value->c_str(), &(factory->loopGainMin_.start), &(factory->loopGainMax_.start));
			parseParameterRampWithVelocity(element, &(factory->loopGainMin_.ramp), &(factory->loopGainMax_.ramp));
			factory->loopGainConcavity_ = c;
			factory->loopGainActive_ = true;		
		}		
		else if(name->compare("AmplitudeFeedbackScaler") == 0) {	// double, time-variant
			parseVelocityPair(value->c_str(), &(factory->amplitudeFeedbackScalerMin_.start), &(factory->amplitudeFeedbackScalerMax_.start));
			parseParameterRampWithVelocity(element, &(factory->amplitudeFeedbackScalerMin_.ramp), &(factory->amplitudeFeedbackScalerMax_.ramp));
			factory->amplitudeFeedbackScalerConcavity_ = c;
			factory->amplitudeFeedbackScalerActive_ = true;
		}		
		else
			cerr << "assignPllSynthParameters() warning: unknown parameter '" << *name << "'\n";
	}
}

void MidiNote::assignNoiseSynthParameters(NoiseSynthFactory *factory, TiXmlElement *element)
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
		int i;
		double c = 0.0;
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "assignNoiseSynthParameters(): Processing name = '" << (*name) << "', value = '" << (*value) << "'\n";
#endif
		if(concavity != NULL)		// Read the concavity attribute if present; if not, assume linear (0)
		{
			stringstream concavityStream(*concavity);
			concavityStream >> c;
		}
		
		// First, check that the name is a parameter we recognize.  Some will have float values, others will be a list of floats.
		// We don't look for a phase offset parameter in the XML file, since that is separately calibrated

		if(name->compare("GlobalAmplitude") == 0) {			// double, time-variant
			parseVelocityPair(value->c_str(), &(factory->globalAmplitudeMin_.start), &(factory->globalAmplitudeMax_.start));
			parseParameterRampWithVelocity(element, &(factory->globalAmplitudeMin_.ramp), &(factory->globalAmplitudeMax_.ramp));
			factory->globalAmplitudeConcavity_ = c;
			factory->globalAmplitudeActive_ = true;			
		}
		else if(name->compare("FilterAmplitudes") == 0) {			// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
			
			factory->filterAmplitudesMin_.clear();					// Form them into paramHolder structs
			factory->filterAmplitudesMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->filterAmplitudesMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->filterAmplitudesMax_.push_back(ph2);
			}
			factory->filterAmplitudesConcavity_ = c;
			factory->filterAmplitudesActive_ = true;
		}
		else if(name->compare("RelativeFrequencies") == 0) {				// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
			
			factory->filterFrequenciesMin_.clear();					// Form them into paramHolder structs
			factory->filterFrequenciesMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->filterFrequenciesMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->filterFrequenciesMax_.push_back(ph2);
			}
			factory->filterFrequenciesConcavity_ = c;
			factory->filterFrequenciesActive_ = true;	
		}
		else if(name->compare("FilterQs") == 0) {					// vector<double>, time-variant
			vector<double> vd1, vd2;
			vector<timedParameter> vtp1, vtp2;
			parseCommaSeparatedValuesWithVelocity(value->c_str(), &vd1, &vd2);		// Collect the starting values
			parseMultiParameterRampWithVelocity(element, &vtp1, &vtp2); // and the ramping values
			
			factory->filterQsMin_.clear();					// Form them into paramHolder structs
			factory->filterQsMax_.clear();
			for(i = 0; i < min(vd1.size(), vd2.size()); i++)
			{
				paramHolder ph1, ph2;
				ph1.start = vd1[i];
				ph2.start = vd2[i];
				if(i < vtp1.size() && i < vtp2.size())					// Possible to have more starting values than ramps
				{
					ph1.ramp = vtp1[i];
					ph2.ramp = vtp2[i];
				}
				factory->filterQsMin_.push_back(ph1);			// Add this paramHolder to the vector
				factory->filterQsMax_.push_back(ph2);
			}
			factory->filterQsConcavity_ = c;
			factory->filterQsActive_ = true;
		}
		else
			cerr << "assignNoiseSynthParameters() warning: unknown parameter '" << *name << "'\n";
	}
}

timedParameter MidiNote::parseParameterRamp(TiXmlElement *element)
{
	TiXmlElement *ramp = element->FirstChildElement("Ramp");
	timedParameter out;
	
	while(ramp != NULL)		// These tags hold ramp values for time-variant parameters
	{
		const string *rampVal = ramp->Attribute((const string)"value");
		const string *rampDur = ramp->Attribute((const string)"duration");
		const string *rampType = ramp->Attribute((const string)"type");
		
		if(rampVal != NULL && rampDur != NULL && rampType != NULL)
		{
			parameterValue param;					
			stringstream s2(*rampDur);
			
			param.nextValue = strtod_with_suffix(rampVal->c_str());
			s2 >> (param.duration);
			if(rampType->compare(0, 3, "lin") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Linear ramp type\n";
#endif
				param.shape = shapeLinear;
			}
			else if(rampType->compare(0, 3, "log") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Logarithmic ramp type\n";
#endif
				param.shape = shapeLogarithmic;
			}
			else if(rampType->compare(0, 2, "st") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Step ramp type\n";
#endif
				param.shape = shapeStep;
			}
			else
			{
				cerr << "parseParameterRamp() warning: unknown type '" << *rampType << "'\n";
				param.shape = shapeLinear;
			}
			
			out.push_back(param);
		}
		else
			cerr << "assignPllSynthParameters() warning: null attributes in Ramp tag\n";
		
		ramp = ramp->NextSiblingElement("Ramp");
	}
	
	return out;
}

// Parse a series of <Ramp> elements whose values and durations may be of "a/b" format, storing the results in
// out1 and out2.

void MidiNote::parseParameterRampWithVelocity(TiXmlElement *element, timedParameter *out1, timedParameter *out2)
{
	TiXmlElement *ramp = element->FirstChildElement("Ramp");
	
	while(ramp != NULL)		// These tags hold ramp values for time-variant parameters
	{
		const string *rampVal = ramp->Attribute((const string)"value");
		const string *rampDur = ramp->Attribute((const string)"duration");
		const string *rampType = ramp->Attribute((const string)"type");
		
		if(rampVal != NULL && rampDur != NULL && rampType != NULL)
		{
			parameterValue param1, param2;				
			
			if(parseVelocityPair(rampVal->c_str(), &param1.nextValue, &param2.nextValue) == 0)
				cerr << "parseParameterRampWithVelocity() warning: found no valid values, using 0\n";
			if(parseVelocityPair(rampDur->c_str(), &param1.duration, &param2.duration) == 0)
				cerr << "parseParameterRampWithVelocity() warning: found no valid durations, using 0\n";
			
			if(rampType->compare(0, 3, "lin") == 0)
			{
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Linear ramp type\n";
#endif
				param1.shape = param2.shape = shapeLinear;
			}
			else if(rampType->compare(0, 3, "log") == 0)
			{
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Logarithmic ramp type\n";
#endif
				param1.shape = param2.shape = shapeLogarithmic;
			}
			else if(rampType->compare(0, 2, "st") == 0)
			{
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Step ramp type\n";
#endif
				param1.shape = param2.shape = shapeStep;
			}
			else
			{
				cerr << "parseParameterRampWithVelocity() warning: unknown type '" << *rampType << "'\n";
				param1.shape = param2.shape = shapeLinear;
			}
			
			if(out1 != NULL)
				out1->push_back(param1);
			if(out2 != NULL)
				out2->push_back(param2);
		}
		else
			cerr << "parseParameterRampWithVelocity() warning: null attributes in Ramp tag\n";
		
		ramp = ramp->NextSiblingElement("Ramp");
	}
}


vector<timedParameter> MidiNote::parseMultiParameterRamp(TiXmlElement *element)
{
	TiXmlElement *ramp = element->FirstChildElement("Ramp");
	vector<timedParameter> out;		// The output is a vector of deque objects.  Each deque holds a list of values to ramp to
	timedParameter emptyTp;			// timedParameter with no parameterValue elements
	bool first = true;
	
	while(ramp != NULL)		// These tags hold ramp values for time-variant parameters
	{
		const string *rampVal = ramp->Attribute((const string)"value");
		const string *rampDur = ramp->Attribute((const string)"duration");
		const string *rampType = ramp->Attribute((const string)"type");
		
		if(rampVal != NULL && rampDur != NULL && rampType != NULL)
		{
			vector<parameterValue> params;					
			vector<double> values = parseCommaSeparatedValues(*rampVal);
			stringstream s(*rampDur);
			int i, size;
			double p;
			int shape;
			
			if(first)										// Need to make the timedParameter vector big enough to hold all params
			{												// Size it to the length of the first ramp value... really each ramp
				while(out.size() < values.size())			// should have the same number of parameters
					out.push_back(emptyTp);
				first = false;
			}
			
			s >> p;											// Each iteration represents one <Ramp> tag.  It will have a single
			if(rampType->compare(0, 3, "lin") == 0)			// duration attribute, a single type attribute, and a list of values.
			{
#ifdef DEBUG_MESSAGES
				cout << "Linear ramp type\n";
#endif
				shape = shapeLinear;
			}
			else if(rampType->compare(0, 3, "log") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Logarithmic ramp type\n";
#endif
				shape = shapeLogarithmic;
			}
			else if(rampType->compare(0, 2, "st") == 0)
			{
#ifdef DEBUG_MESSAGES
				cout << "Step ramp type\n";
#endif
				shape = shapeStep;
			}
			else
			{
				cerr << "parseParameterRamp() warning: unknown type '" << *rampType << "'\n";
				shape = shapeLinear;
			}			
			
			size = (out.size() < values.size() ? out.size() : values.size());	// Minimum of size of output vector, value vector
			
			for(i = 0; i < size; i++)			// Go through each value in the list and add it to the appropriate timedParameter
				out[i].push_back((parameterValue){p,values[i],shape});
		}
		else
			cerr << "assignPllSynthParameters() warning: null attributes in Ramp tag\n";
		
		ramp = ramp->NextSiblingElement("Ramp");
	}
	
	return out;
}

// Parse a list of <Ramp> tags, each of whose values are a comma separated list, where each element of the list is of the form "a/b".
// The outputs are a pair of vectors of timedParameters.  Each timedParameter represents a series of ramp values for one parameter,
// so the vector holds multiple parameters, e.g. multiple harmonic amplitudes.  The trick is keeping the indexing straight.

void MidiNote::parseMultiParameterRampWithVelocity(TiXmlElement *element, vector<timedParameter> *out1, vector<timedParameter> *out2)
{
	TiXmlElement *ramp = element->FirstChildElement("Ramp");	
	timedParameter emptyTp;			// timedParameter with no parameterValue elements
	bool first = true;				
	vector<double> val1, val2;
	
	while(ramp != NULL)		// These tags hold ramp values for time-variant parameters
	{
		const string *rampVal = ramp->Attribute((const string)"value");
		const string *rampDur = ramp->Attribute((const string)"duration");
		const string *rampType = ramp->Attribute((const string)"type");
		
		if(rampVal != NULL && rampDur != NULL && rampType != NULL)
		{
			double dur1, dur2;
			int i, size, shape;
			
			// Each <Ramp> tag holds:
			//    a list of pairs of values: "0/1, 2/3, 4/5"
			//    a pair of durations: "2/3"
			//    a single shape: "linear"
			
			parseCommaSeparatedValuesWithVelocity(rampVal->c_str(), &val1, &val2);	// val1 and val2 will have same size
			parseVelocityPair(rampDur->c_str(), &dur1, &dur2);
			
			if(first)										// Need to make the timedParameter vector big enough to hold all params
			{												// Size it to the length of the first ramp value... really each ramp
				if(out1 != NULL)							// should have the same number of parameters
					while(out1->size() < val1.size())			
						out1->push_back(emptyTp);
				if(out2 != NULL)
					while(out2->size() < val1.size())
						out2->push_back(emptyTp);
				first = false;
			}
															// Each iteration represents one <Ramp> tag.  It will have a single
			if(rampType->compare(0, 3, "lin") == 0)			// duration attribute, a single type attribute, and a list of values.
			{
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Linear ramp type\n";
#endif
				shape = shapeLinear;
			}
			else if(rampType->compare(0, 3, "log") == 0)
			{
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Logarithmic ramp type\n";
#endif
				shape = shapeLogarithmic;
			}
			else if(rampType->compare(0, 2, "st") == 0)
			{
#ifdef DEBUG_MESSAGES_EXTRA
				cout << "Step ramp type\n";
#endif
				shape = shapeStep;
			}
			else
			{
				cerr << "parseParameterRamp() warning: unknown type '" << *rampType << "'\n";
				shape = shapeLinear;
			}			
			
			size = val1.size();					// Minimum of size of output vectors, value vector
			if(out1 != NULL)
				if(out1->size() < size) size = out1->size();
			if(out2 != NULL)
				if(out2->size() < size) size = out2->size();
			
			for(i = 0; i < size; i++)			// Go through each value in the list and add it to the appropriate timedParameter
			{
				if(out1 != NULL)
					(*out1)[i].push_back((parameterValue){dur1,val1[i],shape});
				if(out2 != NULL)
					(*out2)[i].push_back((parameterValue){dur2,val2[i],shape});
			}
			
			val1.clear();
			val2.clear();
		}
		else
			cerr << "assignPllSynthParameters() warning: null attributes in Ramp tag\n";
		
		ramp = ramp->NextSiblingElement("Ramp");
	}
}

vector<double> MidiNote::parseCommaSeparatedValues(const string& inString)
{
	vector<double> out;
	char *str, *strOrig, *ap;

	str = strOrig = strdup(inString.c_str());
	
	// Parse a string of the format "0, 1.3, -1.23, 4" containing a list of double values, and put the values into a vector
	// Strings of the form "1,,,,,,3,4" are acceptable-- empty values default to 0
	
	while((ap = strsep(&str, ",; \t")) != NULL)
	{
		if((*ap) == '\0')	// Empty field-- defaults to 0
			out.push_back(0.0);
		else				// Extract the double value
			out.push_back(strtod_with_suffix(ap));
	}
	
	free(strOrig);	// This pointer won't move like str
	return out;
}

// Parse a string of the format "1/2,2.5/2.7,0/100" storing the results in two vectors

void MidiNote::parseCommaSeparatedValuesWithVelocity(const char *inString, vector<double> *out1, vector<double> *out2)
{
	char *str, *strOrig, *ap;
	
	str = strOrig = strdup(inString);
	
	// Parse a string of the format "0, 1.3, -1.23, 4" containing a list of double values, and put the values into a vector
	// Strings of the form "1,,,,,,3,4" are acceptable-- empty values default to 0
	
	while((ap = strsep(&str, ",; \t")) != NULL)
	{
		if((*ap) == '\0')	// Empty field-- defaults to 0
		{
			out1->push_back(0.0);
			out2->push_back(0.0);
		}
		else
		{	// Extract the double value
			double a,b;
			parseVelocityPair(ap, &a, &b);
			out1->push_back(a);
			out2->push_back(b);
		}
	}
	
	free(strOrig);	// This pointer won't move like str
}

// Parses strings of the format "a/b" indicating a range of velocity-sensitive values.
// Stores the two values in outA and outB; if the string only contains one number, outA = outB.
// Returns the number of values found (0, 1, 2)
int MidiNote::parseVelocityPair(const char *inString, double *outA, double *outB)
{
	char *str, *strOrig, *ap = NULL, *ap2 = NULL;
	double out[2];
	int count = 0;
	
	str = strOrig = strdup(inString);
	
	if((ap = strsep(&str, "/")) != NULL)	// Look for up to two tokens
		ap2 = strsep(&str, "/");
	
	if(ap != NULL)
		if((*ap) != '\0')
			out[count++] = strtod_with_suffix(ap);
	if(ap2 != NULL)
		if((*ap2) != '\0')
			out[count++] = strtod_with_suffix(ap2);
	
	if(count == 2)
	{
		*outA = out[0];
		*outB = out[1];
	}
	else if(count == 1)
	{
		*outA = *outB = out[0];
	}
	else
		*outA = *outB = 0.0;						// No value found
	
	free(strOrig);	// This pointer won't move like str
	return count;
}

// This extends the usual strtod method to allow dB values (possibly other suffixes in the future)

double MidiNote::strtod_with_suffix( const char * str )
{
	char *endString;
	double out;
	
	out = strtod(str, &endString);
	if(!strncmp(endString, "dB", 2) || !strncmp(endString, "db", 2))
	{
		out = pow(10.0, out/20.0);		// Convert dB to linear scale
	}	

	return out;
}

#pragma mark Factory Methods

double MidiNote::SynthBaseFactory::transeg(double val1, double val2, double concavity, double velocity)
{
	// Given a low and high value, a concavity and a velocity, return a normalized value
	// This operates similar to csound's transeg opcode.  concavity > 0 produces a slowly rising (concave) curve
	// where concavity < 0 produces a convex curve.  concavity = 0 produces a linear curve.
	
	if(val1 == val2) 
		return val1;
	
	double out;
	double scaledVelocity = velocity / 127.0;		// Normalize to 0-1
	
	if(concavity == 0.0)
		out = val1 + scaledVelocity*(val2 - val1);
	else
		out = val1 + (val2 - val1)*(1.0 - exp(scaledVelocity * concavity))/(1.0 - exp(concavity));
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "transeg: input (" << val1 << ", " << val2 << ") conc. " << concavity << " vel. " << velocity << " output " << out << endl;
#endif
	return out;
}

PllSynth* MidiNote::PllSynthFactory::createSynth(int note, int velocity, float velocityCurvature, float amplitudeOffset)
{
	PllSynth *out = new PllSynth(sampleRate_);		// Create a new synth object
	float baseFreq = controller_->midiNoteToFrequency(note);
	int i, j;
	double start;
	parameterValue pval;
	timedParameter ramp;
	vector<double> vd;
	vector<timedParameter> vtp;
	
	double curvedVelocity = transeg(0.0, 127.0, velocityCurvature, (double)velocity);
#ifdef DEBUG_MESSAGES_EXTRA
	cout << "Curved velocity: " << curvedVelocity << endl;
#endif

	// Set non-ramping, non-velocity-sensitive parameters
	if(useAmplitudeFeedbackActive_)
		out->setUseAmplitudeFeedback(useAmplitudeFeedback_);
	if(useInterferenceRejectionActive_)
		out->setUseInterferenceRejection(useInterferenceRejection_);
	if(filterQActive_)
		out->setFilterQ(filterQ_);
	if(loopFilterPoleActive_)
		out->setLoopFilterPole(loopFilterPole_);
	if(loopFilterZeroActive_)
		out->setLoopFilterZero(loopFilterZero_);
	
	// Set single velocity-sensitive, ramping parameters
	if(globalAmplitudeActive_)
	{
		start = amplitudeOffset*transeg(globalAmplitudeMin_.start, globalAmplitudeMax_.start, globalAmplitudeConcavity_, curvedVelocity);
		for(i = 0; i < min(globalAmplitudeMin_.ramp.size(), globalAmplitudeMax_.ramp.size()); i++)
		{
			pval.nextValue = amplitudeOffset*transeg(globalAmplitudeMin_.ramp[i].nextValue, globalAmplitudeMax_.ramp[i].nextValue,
									 globalAmplitudeConcavity_, curvedVelocity);
			pval.duration = transeg(globalAmplitudeMin_.ramp[i].duration, globalAmplitudeMax_.ramp[i].duration,
									globalAmplitudeConcavity_, curvedVelocity);
			pval.shape = globalAmplitudeMin_.ramp[i].shape;
			ramp.push_back(pval);
		}
		out->setGlobalAmplitude(start, ramp);
		ramp.clear();
	}
	else
	{
		start = amplitudeOffset*0.1;	// Default amplitude value includes calibration data
		ramp.clear();
		
		out->setGlobalAmplitude(start, ramp);
	}
	if(loopGainActive_)
	{
		start = transeg(loopGainMin_.start, loopGainMax_.start, loopGainConcavity_, curvedVelocity);
		for(i = 0; i < min(loopGainMin_.ramp.size(), loopGainMax_.ramp.size()); i++)
		{
			pval.nextValue = transeg(loopGainMin_.ramp[i].nextValue, loopGainMax_.ramp[i].nextValue,
									 loopGainConcavity_, curvedVelocity);
			pval.duration = transeg(loopGainMin_.ramp[i].duration, loopGainMax_.ramp[i].duration,
									loopGainConcavity_, curvedVelocity);
			pval.shape = loopGainMin_.ramp[i].shape;
			ramp.push_back(pval);
		}
		out->setLoopGain(start, ramp);
		ramp.clear();
	}
	if(amplitudeFeedbackScalerActive_)
	{
		start = transeg(amplitudeFeedbackScalerMin_.start, amplitudeFeedbackScalerMax_.start, amplitudeFeedbackScalerConcavity_, curvedVelocity);
		for(i = 0; i < min(amplitudeFeedbackScalerMin_.ramp.size(), amplitudeFeedbackScalerMax_.ramp.size()); i++)
		{
			pval.nextValue = transeg(amplitudeFeedbackScalerMin_.ramp[i].nextValue, amplitudeFeedbackScalerMax_.ramp[i].nextValue,
									 amplitudeFeedbackScalerConcavity_, curvedVelocity);
			pval.duration = transeg(amplitudeFeedbackScalerMin_.ramp[i].duration, amplitudeFeedbackScalerMax_.ramp[i].duration,
									amplitudeFeedbackScalerConcavity_, curvedVelocity);
			pval.shape = amplitudeFeedbackScalerMin_.ramp[i].shape;
			ramp.push_back(pval);
		}
		out->setAmplitudeFeedbackScaler(start, ramp);
		ramp.clear();
	}		
	
	// Set vector ramping parameters (also velocity sensitive)
	if(inputGainsActive_)
	{
		for(j = 0; j < min(inputGainsMin_.size(), inputGainsMax_.size()); j++)
		{
			start = transeg(inputGainsMin_[j].start, inputGainsMax_[j].start, inputGainsConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(inputGainsMin_[j].ramp.size(), inputGainsMax_[j].ramp.size()); i++)
			{
				pval.nextValue = transeg(inputGainsMin_[j].ramp[i].nextValue, inputGainsMax_[j].ramp[i].nextValue,
										 inputGainsConcavity_, curvedVelocity);
				pval.duration = transeg(inputGainsMin_[j].ramp[i].duration, inputGainsMax_[j].ramp[i].duration,
										inputGainsConcavity_, curvedVelocity);
				pval.shape = inputGainsMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setInputGains(vd, vtp);
		vd.clear();
		vtp.clear();
	}
	if(inputDelaysActive_)
	{
		for(j = 0; j < min(inputDelaysMin_.size(), inputDelaysMax_.size()); j++)
		{
			start = transeg(inputDelaysMin_[j].start, inputDelaysMax_[j].start, inputDelaysConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(inputDelaysMin_[j].ramp.size(), inputDelaysMax_[j].ramp.size()); i++)
			{
				pval.nextValue = transeg(inputDelaysMin_[j].ramp[i].nextValue, inputDelaysMax_[j].ramp[i].nextValue,
										 inputDelaysConcavity_, curvedVelocity);
				pval.duration = transeg(inputDelaysMin_[j].ramp[i].duration, inputDelaysMax_[j].ramp[i].duration,
										inputDelaysConcavity_, curvedVelocity);
				pval.shape = inputDelaysMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setInputDelays(vd, vtp);
		vd.clear();
		vtp.clear();
	}	
	if(harmonicAmplitudesActive_)
	{
		for(j = 0; j < min(harmonicAmplitudesMin_.size(), harmonicAmplitudesMax_.size()); j++)
		{
			start = transeg(harmonicAmplitudesMin_[j].start, harmonicAmplitudesMax_[j].start, harmonicAmplitudesConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(harmonicAmplitudesMin_[j].ramp.size(), harmonicAmplitudesMax_[j].ramp.size()); i++)
			{
				pval.nextValue = transeg(harmonicAmplitudesMin_[j].ramp[i].nextValue, harmonicAmplitudesMax_[j].ramp[i].nextValue,
										 harmonicAmplitudesConcavity_, curvedVelocity);
				pval.duration = transeg(harmonicAmplitudesMin_[j].ramp[i].duration, harmonicAmplitudesMax_[j].ramp[i].duration,
										harmonicAmplitudesConcavity_, curvedVelocity);
				pval.shape = harmonicAmplitudesMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setHarmonicAmplitudes(vd, vtp);
		vd.clear();
		vtp.clear();
	}	
	if(harmonicPhasesActive_)
	{
		for(j = 0; j < min(harmonicPhasesMin_.size(), harmonicPhasesMax_.size()); j++)
		{
			start = transeg(harmonicPhasesMin_[j].start, harmonicPhasesMax_[j].start, harmonicPhasesConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(harmonicPhasesMin_[j].ramp.size(), harmonicPhasesMax_[j].ramp.size()); i++)
			{
				pval.nextValue = transeg(harmonicPhasesMin_[j].ramp[i].nextValue, harmonicPhasesMax_[j].ramp[i].nextValue,
										 harmonicPhasesConcavity_, curvedVelocity);
				pval.duration = transeg(harmonicPhasesMin_[j].ramp[i].duration, harmonicPhasesMax_[j].ramp[i].duration,
										harmonicPhasesConcavity_, curvedVelocity);
				pval.shape = harmonicPhasesMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setHarmonicPhases(vd, vtp);
		vd.clear();
		vtp.clear();
	}
	
	// Set center frequency, which depends on both note and velocity
	if(relativeFrequencyActive_)
	{
		start = baseFreq*transeg(relativeFrequencyMin_.start, relativeFrequencyMax_.start, relativeFrequencyConcavity_, curvedVelocity);
		for(i = 0; i < min(relativeFrequencyMin_.ramp.size(), relativeFrequencyMax_.ramp.size()); i++)
		{
			pval.nextValue = baseFreq*transeg(relativeFrequencyMin_.ramp[i].nextValue, relativeFrequencyMax_.ramp[i].nextValue,
									 relativeFrequencyConcavity_, curvedVelocity);
			pval.duration = transeg(relativeFrequencyMin_.ramp[i].duration, relativeFrequencyMax_.ramp[i].duration,
									relativeFrequencyConcavity_, curvedVelocity);
			pval.shape = relativeFrequencyMin_.ramp[i].shape;
			ramp.push_back(pval);
		}
		out->setCenterFrequency(start, ramp);
		ramp.clear();
	}
	else
	{
		ramp.clear();
		out->setCenterFrequency(baseFreq, ramp);
	}
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << *out;
#endif
	return out;
}

NoiseSynth* MidiNote::NoiseSynthFactory::createSynth(int note, int velocity, float velocityCurvature, float amplitudeOffset)
{
	NoiseSynth *out = new NoiseSynth(sampleRate_);		// Create a new synth object	
	
	float baseFreq = controller_->midiNoteToFrequency(note);
	int i, j;
	double start;
	parameterValue pval;
	timedParameter ramp;
	vector<double> vd;
	vector<timedParameter> vtp;
	
	double curvedVelocity = transeg(0.0, 127.0, velocityCurvature, (double)velocity);
#ifdef DEBUG_MESSAGES
	cout << "Curved velocity: " << curvedVelocity << endl;
#endif
	
	// Set single velocity-sensitive, ramping parameters
	if(globalAmplitudeActive_)
	{
		start = amplitudeOffset*transeg(globalAmplitudeMin_.start, globalAmplitudeMax_.start, globalAmplitudeConcavity_, curvedVelocity);
		for(i = 0; i < min(globalAmplitudeMin_.ramp.size(), globalAmplitudeMax_.ramp.size()); i++)
		{
			pval.nextValue = amplitudeOffset*transeg(globalAmplitudeMin_.ramp[i].nextValue, globalAmplitudeMax_.ramp[i].nextValue,
									 globalAmplitudeConcavity_, curvedVelocity);
			pval.duration = transeg(globalAmplitudeMin_.ramp[i].duration, globalAmplitudeMax_.ramp[i].duration,
									globalAmplitudeConcavity_, curvedVelocity);
			pval.shape = globalAmplitudeMin_.ramp[i].shape;
			ramp.push_back(pval);
		}
		out->setGlobalAmplitude(start, ramp);
		ramp.clear();
	}	

	if(filterFrequenciesActive_)		// Normal filter frequencies to base frequency of the note
	{
		for(j = 0; j < min(filterFrequenciesMin_.size(), filterFrequenciesMax_.size()); j++)
		{
			start = baseFreq*transeg(filterFrequenciesMin_[j].start, filterFrequenciesMax_[j].start, filterFrequenciesConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(filterFrequenciesMin_[j].ramp.size(), filterFrequenciesMax_[j].ramp.size()); i++)
			{
				pval.nextValue = baseFreq*transeg(filterFrequenciesMin_[j].ramp[i].nextValue, filterFrequenciesMax_[j].ramp[i].nextValue,
										 filterFrequenciesConcavity_, curvedVelocity);
				pval.duration = transeg(filterFrequenciesMin_[j].ramp[i].duration, filterFrequenciesMax_[j].ramp[i].duration,
										filterFrequenciesConcavity_, curvedVelocity);
				pval.shape = filterFrequenciesMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setFilterFrequencies(vd, vtp);
		vd.clear();
		vtp.clear();
	}	
	// else do nothing-- no filters = plain white noise
	
	if(filterQsActive_)
	{
		for(j = 0; j < min(filterQsMin_.size(), filterQsMax_.size()); j++)
		{
			start = transeg(filterQsMin_[j].start, filterQsMax_[j].start, filterQsConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(filterQsMin_[j].ramp.size(), filterQsMax_[j].ramp.size()); i++)
			{
				pval.nextValue = transeg(filterQsMin_[j].ramp[i].nextValue, filterQsMax_[j].ramp[i].nextValue,
										 filterQsConcavity_, curvedVelocity);
				pval.duration = transeg(filterQsMin_[j].ramp[i].duration, filterQsMax_[j].ramp[i].duration,
										filterQsConcavity_, curvedVelocity);
				pval.shape = filterQsMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setFilterQs(vd, vtp);
		vd.clear();
		vtp.clear();
	}	
	if(filterAmplitudesActive_)
	{
		for(j = 0; j < min(filterAmplitudesMin_.size(), filterAmplitudesMax_.size()); j++)
		{
			start = transeg(filterAmplitudesMin_[j].start, filterAmplitudesMax_[j].start, filterAmplitudesConcavity_, curvedVelocity);
			vd.push_back(start);
			for(i = 0; i < min(filterAmplitudesMin_[j].ramp.size(), filterAmplitudesMax_[j].ramp.size()); i++)
			{
				pval.nextValue = transeg(filterAmplitudesMin_[j].ramp[i].nextValue, filterAmplitudesMax_[j].ramp[i].nextValue,
										 filterAmplitudesConcavity_, curvedVelocity);
				pval.duration = transeg(filterAmplitudesMin_[j].ramp[i].duration, filterAmplitudesMax_[j].ramp[i].duration,
										filterAmplitudesConcavity_, curvedVelocity);
				pval.shape = filterAmplitudesMin_[j].ramp[i].shape;
				ramp.push_back(pval);
			}			
			vtp.push_back(ramp);
			ramp.clear();
		}
		out->setFilterAmplitudes(vd, vtp);
		vd.clear();
		vtp.clear();
	}		
	
#ifdef DEBUG_MESSAGES_EXTRA
	cout << *out;
#endif
	return out;
}

#pragma mark CalibratorNote

CalibratorNote::CalibratorNote(MidiController *controller, AudioRender *render) : MidiNote(controller, render) 
{ 
#ifdef DEBUG_ALLOCATION
	cout << "*** CalibratorNote\n";
#endif
	
	// Initialize these to values that will never come up
	phaseControl_ = amplitudeControl_ = phaseControlChannel_ = amplitudeControlChannel_ = 255;
	synths_.clear();
	factories_.clear();
	
	PllSynth *calibratorSynth = new PllSynth(render_->sampleRate());
	timedParameter emptyParameter;
	
	// Set some important default values.  Actual frequency will be set at note-on time; phase offset and global
	// amplitude will be adjusted by controls
	
	calibratorGlobalAmplitude_ = 0.1;		// -20dB volume
	
	calibratorSynth->setLoopGain(100000.0, emptyParameter);
	calibratorSynth->setUseAmplitudeFeedback(false);
	calibratorSynth->setUseInterferenceRejection(false);
	calibratorSynth->setFilterQ(50.0);
	calibratorSynth->setGlobalAmplitude(calibratorGlobalAmplitude_, emptyParameter);
	calibratorSynth->setPhaseOffset(0.0, emptyParameter);
	
	synths_.push_back(calibratorSynth);
}

int CalibratorNote::parseXml(TiXmlElement *baseElement)
{
	// Load from XML table the controller values to use.  Format:
	//   <Control name="phaseOffset" id="10" channel="1"/>
	//   <Control name="amplitudeOffset" id="11" channel="1"/>
	//   <Control name="globalAmplitude" id="12" channel="1"/>
	// The three controls above reflect phase offset, amplitude offset, and a global calibration amplitude which doesn't
	// get saved (only used to find a comfortable volume for calibrating).
	// There will always be one synth within this note.  Its parameters are not controllable.

	TiXmlElement *element = baseElement->FirstChildElement("Control");
	const string *name = baseElement->Attribute((const string)"name");
	
	if(name != NULL)
		name_ = *name;
	
	while(element != NULL)
	{
		const string *controlName = element->Attribute((const string)"name");
		const string *controlId = element->Attribute((const string)"id");
		const string *controlChannel = element->Attribute((const string)"channel");
		
		if(controlId == NULL || controlName == NULL)
			cerr << "CalibratorNote warning: missing Control 'id' or 'name' tag\n";
		else
		{
			if(*controlName == "phaseOffset")
			{
				stringstream idStream(*controlId);
				idStream >> phaseControl_;
				if(controlChannel != NULL)
				{
					stringstream channelStream(*controlChannel);
					channelStream >> phaseControlChannel_;
				}
				else
					phaseControlChannel_ = 0;
			}
			else if(*controlName == "amplitudeOffset")
			{
				stringstream idStream(*controlId);
				idStream >> amplitudeControl_;
				if(controlChannel != NULL)
				{
					stringstream channelStream(*controlChannel);
					channelStream >> amplitudeControlChannel_;
				}
				else
					amplitudeControlChannel_ = 0;				
			}
			else
			{
#ifdef DEBUG_MESSAGES
				cout << "CalibratorNote::parseXml(): unknown control name '" << controlName << "'\n";
#endif
			}	
		}
			
		element = element->NextSiblingElement("Control");
	}
	
	return 0;
}

void CalibratorNote::begin(bool damperLifted)
{
	MidiNote::begin(damperLifted);
	
	if(isRunning_)
		controller_->addEventListener(this, true, false, false, false);
}

void CalibratorNote::midiControlChange(unsigned char channel, unsigned char control, unsigned char value)
{	
	if(!isRunning_)
		return;
	
	// Listen for special controls that adjust phase offset, amplitude offset, and save values
	// Call up to MidiNote::midiControlChange to handle the damper and sostenuto pedal issues
	
	if((unsigned int)control == phaseControl_ && (unsigned int)channel == phaseControlChannel_)
	{
		// When the control for phaseOffset changes, save value and send a setPhaseOffset message to synth
		// Control range maps to a phase offset of 0-1

		calibrateSetPhase((float)value/127.0);
	}
	else if((unsigned int)control == amplitudeControl_ && (unsigned int)channel == amplitudeControlChannel_)
	{
		// When the control for amplitudeAdjust changes, save value and send a setGlobalAmplitude message to synth
		// Control range maps to amplitude offset of -12dB to 6dB (0.25 to 2.0), with the lower half of the range having
		// more response, to attenuate particularly loud notes
		
		float amplitudeOffset;
		
		if(value >= 64)
			amplitudeOffset = (float)pow(2.0, ((double)value - 64.0)/64.0);
		else
			amplitudeOffset = (float)pow(4.0, ((double)value - 64.0)/64.0);

		calibrateSetAmplitude(amplitudeOffset);
	}
	else
		MidiNote::midiControlChange(channel, control, value);
}

// Set the phase offset for this particular note.  Called either from midiControlChange or by OSC.

void CalibratorNote::calibrateSetPhase(float phaseOffset)
{
	timedParameter emptyParam;
	
	if(synths_.size() > 0)
		((PllSynth*)synths_[0])->setPhaseOffset(phaseOffset, emptyParam);
#ifdef DEBUG_MESSAGES
	cout << "Phase offset = " << phaseOffset << endl;
#endif
	// Save this value in the controller table
	if(midiNote_ >= 0 && midiNote_ < 128)
		controller_->phaseOffsets_[midiNote_] = phaseOffset;
	else
		cerr << "warning: phase offset value not saved (midiNote_ = " << midiNote_ << ")\n";
}

// Set the amplitude offset for this particular note.  Called either from midiControlChange or by OSC.

void CalibratorNote::calibrateSetAmplitude(float amplitudeOffset)
{
	timedParameter emptyParam;
	
	if(synths_.size() > 0)
		((PllSynth*)synths_[0])->setGlobalAmplitude(calibratorGlobalAmplitude_*amplitudeOffset, emptyParam);		
#ifdef DEBUG_MESSAGES
	cout << "Amplitude offset = " << amplitudeOffset << endl;
#endif		
	
	// Save this value in the controller table
	if(midiNote_ >= 0 && midiNote_ < 128)
		controller_->amplitudeOffsets_[midiNote_] = amplitudeOffset;
	else
		cerr << "warning: amplitude offset value not saved (midiNote_ = " << midiNote_ << ")\n";
}

CalibratorNote* CalibratorNote::createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key, 
										   int priority, int velocity, float phaseOffset, float amplitudeOffset)
{
	// Essentially duplicate this note.  This means the synth in the prototype note is redundant, but perhaps later we'll want
	// to control its parameters from the XML file, so we won't worry about the extra memory too much.
	
	CalibratorNote *out = new CalibratorNote(controller_, render_);
	int i;
	float baseFreq;
	timedParameter emptyParam;
	
	out->phaseControl_ = phaseControl_;
	out->phaseControlChannel_ = phaseControlChannel_;
	out->amplitudeControl_ = amplitudeControl_;
	out->amplitudeControlChannel_ = amplitudeControlChannel_;
	out->calibratorGlobalAmplitude_ = calibratorGlobalAmplitude_;
	
	out->setPerformanceParameters(audioChannel, mrpChannel, midiNote, midiChannel, pianoString, key, priority, velocity);
	baseFreq = controller_->midiNoteToFrequency(midiNote);				// Find the center frequency for this note
	
	for(i = 0; i < out->synths_.size(); i++)
		delete out->synths_[i];
	out->synths_.clear();
	
	// Tell each synth to update its center frequency; there should only be one synth but who knows....
	for(i = 0; i < synths_.size(); i++)
	{
		if(typeid(*synths_[i]) != typeid(PllSynth))
			continue;
		PllSynth *newSynth = new PllSynth(*(PllSynth *)synths_[i]);
		
		newSynth->setPerformanceParameters(render_->numInputChannels(), render_->numOutputChannels(), audioChannel);
		newSynth->setCenterFrequency(baseFreq, emptyParam);
		newSynth->setGlobalAmplitude(calibratorGlobalAmplitude_*amplitudeOffset, emptyParam);
		newSynth->setPhaseOffset(phaseOffset, emptyParam);
		
		out->synths_.push_back(newSynth);
	}	
	
	return out;
}

// This method registers for specific OSC paths when the OscController object is set.  We need a reference to the
// controller so we can unregister on destruction.  Unregistering is handled by the OscHandler destructor.

void CalibratorNote::setOscController(OscController *c)
{
	if(c == NULL)
		return;
	
	string phasePath("/ui/cal/phase"), volumePath("/ui/cal/volume");
	
	OscHandler::setOscController(c);
	
	// Send a message announcing the current note, to update the UI
	if(midiNote_ >= 21 && midiNote_ <= 108)
	{
		oscController_->sendMessage("/ui/cal/currentnote", "s", kNoteNames[midiNote_-21], LO_ARGS_END);
	}
	
	addOscListener(phasePath);
	addOscListener(volumePath);
}

// This method is called by the OscController when it receives a message we've registered for.  Use it to change
// calibration values.  Returns true if message was handled.

bool CalibratorNote::oscHandlerMethod(const char *path, const char *types, int numValues, lo_arg **values, void *data)
{
	if(!strcmp(path, "/ui/cal/phase") && numValues >= 1)
	{
		calibrateSetPhase(values[0]->f);
		return true;
	}
	if(!strcmp(path, "/ui/cal/volume") && numValues >= 1)
	{
		float value = values[0]->f, amplitudeOffset;
		
		if(value >= 0.5)
			amplitudeOffset = powf(2.0, (value - 0.5)/0.5);
		else
			amplitudeOffset = powf(4.0, (value - 0.5)/0.5);		
		
		calibrateSetAmplitude(amplitudeOffset);
		return true;
	}
	return false;
}

CalibratorNote::~CalibratorNote()
{
#ifdef DEBUG_ALLOCATION
	cout << "*** ~CalibratorNote\n";
#endif
	// Delete any special stuff we allocate that's different than MidiNote
}

#pragma mark ResonanceNote

ResonanceNote::ResonanceNote(MidiController *controller, AudioRender *render) : MidiNote(controller, render) 
{ 
#ifdef DEBUG_ALLOCATION
	cout << "*** ResonanceNote\n";
#endif
	
	synths_.clear();
	factories_.clear();
	
	// Set up the set of harmonically-related notes we should listen to
	harmonicallyRelated_.insert(12);	// 2nd = octave
	harmonicallyRelated_.insert(19);	// 3rd = octave + P5
	harmonicallyRelated_.insert(24);	// 4th = 2 octaves
	harmonicallyRelated_.insert(28);	// 5th = 2 octaves + M3
	harmonicallyRelated_.insert(31);	// 6th = 2 octaves + P5 
	harmonicallyRelated_.insert(36);	// 8th = 3 octaves (7th doesn't respond on piano)
	harmonicallyRelated_.insert(38);	// 9th = 3 octaves + M2
	harmonicallyRelated_.insert(40);	// 10th = 3 octaves + M3
	harmonicallyRelated_.insert(43);	// 12th = 3 octaves + P5 (11th doesn't respond)
}

// Subclass this to listen for MIDI note activity
void ResonanceNote::begin(bool damperLifted)
{
	MidiNote::begin(damperLifted);
	
	if(isRunning_)
		controller_->addEventListener(this, false, false, false, true);
}

// Parse an XML data structure to get all the parameters we need.  This is used to get data for the ResonanceSynth object

int ResonanceNote::parseXml(TiXmlElement *baseElement)
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
	
	while(synthElement != NULL)
	{
		const string *synthClass = synthElement->Attribute((const string)"class");	// Attribute tells us the kind of synth
		
		if(synthClass == NULL)		// If we can't find a class attribute, skip it
		{
			cerr << "ResonanceNote::parseXml() warning: Synth object with no class\n";
			synthElement = synthElement->NextSiblingElement("Synth");
			continue;
		}
		if(synthClass->compare("ResonanceSynth") == 0 )
		{
			// This is the only type of synth we respond to (for now) in this class.
			
			// Each synth holds a collection of parameters
			TiXmlElement *paramElement = synthElement->FirstChildElement("Parameter");
			ResonanceSynth *resSynth = new ResonanceSynth(render_->sampleRate());

			while(paramElement != NULL)
			{
				assignResonanceSynthParameters(resSynth, paramElement);
				paramElement = paramElement->NextSiblingElement("Parameter");
			}
			
			synths_.push_back(resSynth);			
		}
		else
		{
			cerr << "ResonanceNote::parseXml() warning: Unknown Synth class '" << *synthClass << "'\n";
		}
		
		numSynths++;
		synthElement = synthElement->NextSiblingElement("Synth");
	}
	
	if(numSynths == 0)
		cerr << "ResonanceNote::parseXml() warning: no Synths found\n";	
	
	return 0;
}


// This gets called whenever a new MIDI note comes in.  We care only about note on messages, not note off.
// If it's one of the harmonics of this note, send it on to the synthesizer.
// TODO: Also if one of its harmonics overlaps with a harmonic of the note??

void ResonanceNote::midiNoteEvent(unsigned char status, unsigned char note, unsigned char velocity)
{
	// Check that the status byte is a note on message from our same channel
	unsigned char channel = status & 0x0F;
	unsigned char message = status & 0xF0;
	
	if(message != MidiController::MESSAGE_NOTEON || channel != midiChannel_)
		return;

	// See if the incoming note is harmonically related to the pitch of the active note
	if(harmonicallyRelated_.count(note - midiNote_) > 0)
	{
		if(synths_.size() > 0)
		{
			float frequency = controller_->midiNoteToFrequency(note);
			float amplitude = (float)velocity / 127.0;		// FIXME: need a better amplitude calculation
			
			if(typeid(*synths_[0]) == typeid(ResonanceSynth))
				((ResonanceSynth *)synths_[0])->addHarmonic(note, frequency, amplitude);
		}
	}
}

ResonanceNote* ResonanceNote::createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, int pianoString, unsigned int key,
										 int priority, int velocity, float phaseOffset, float amplitudeOffset)
{	
	ResonanceNote *out = new ResonanceNote(controller_, render_);
	int i;
	
	out->harmonicallyRelated_ = harmonicallyRelated_;
	
	out->setPerformanceParameters(audioChannel, mrpChannel, midiNote, midiChannel, pianoString, key, priority, velocity);
	
	for(i = 0; i < synths_.size(); i++)
	{
		if(typeid(*synths_[i]) != typeid(ResonanceSynth))
			continue;
		
		ResonanceSynth *newSynth = new ResonanceSynth(*(ResonanceSynth *)synths_[i]);
		newSynth->setPerformanceParameters(render_->numInputChannels(), render_->numOutputChannels(), audioChannel);
		
		out->synths_.push_back(newSynth);
	}
	
	return out;	
}

ResonanceNote::~ResonanceNote()
{
#ifdef DEBUG_ALLOCATION
	cout << "*** ~ResonanceNote\n";
#endif
	// Delete any special stuff we allocate that's different than MidiNote
}

// Private utility method that assigns parameter values to a synth based on XML data

void ResonanceNote::assignResonanceSynthParameters(ResonanceSynth *synth, TiXmlElement *element)
{
	// Each Parameter holds three attribute tags: name, value, and (optionally) concavity for velocity-sensitive parameters
	
	const string *name = element->Attribute((const string)"name");
	const string *value = element->Attribute((const string)"value");
	const string *concavity = element->Attribute((const string)"concavity");	// We don't actually use this here
	
	if(value != NULL && name != NULL)	
	{
		stringstream valueStream(*value);
		timedParameter tp;
		double c = 0.0, d;
		
#ifdef DEBUG_MESSAGES
		cout << "assignPitchTrackSynthParameters(): Processing name = '" << (*name) << "', value = '" << (*value) << "'\n";
#endif
		if(concavity != NULL)		// Read the concavity attribute if present; if not, assume linear (0)
		{
			stringstream concavityStream(*concavity);
			concavityStream >> c;
		}
		
		// First, check that the name is a parameter we recognize.  Some will have float values, others will be a list of floats.
		// We don't look for a phase offset parameter in the XML file, since that is separately calibrated
		if(name->compare("GlobalAmplitude") == 0) {					// double, time-variant
			valueStream >> d;
			tp = parseParameterRamp(element);
			synth->setGlobalAmplitude(d, tp);
		}
		else if(name->compare("Rolloff") == 0) {					// double, time-variant
			valueStream >> d;
			tp = parseParameterRamp(element);
			synth->setHarmonicRolloff(d, tp);	
		}	
		else if(name->compare("DecayRate") == 0) {					// double, time-variant
			valueStream >> d;
			tp = parseParameterRamp(element);
			synth->setDecayRate(d, tp);	
		}			
		else if(name->compare("Mono") == 0) {						// bool, time-invariant
			bool b;
			valueStream >> boolalpha >> b;
			synth->setMono(b);
		}
		else
			cerr << "assignPitchTrackSynthParameters() warning: unknown parameter '" << *name << "'\n";
	}
}
