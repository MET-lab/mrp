/*
 *  realtimenote.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 2/14/10.
 *  Copyright 2010 __MyCompanyName__. All rights reserved.
 *
 */

#include "realtimenote.h"

#pragma mark RealTimeMidiNote

int RealTimeMidiNote::parseXml(TiXmlElement *baseElement)
{
	// Here, call the MidiNote superclass to get all the regular parameters (maybe).  But then we also want the following:
	
	if(MidiNote::parseXml(baseElement))
		return 1;

	TiXmlElement *rtqElement, *qualityElement;
	
	// Look for a <RealTimeQualities> tag which holds the data we want
	
	rtqElement = baseElement->FirstChildElement("RealTimeQualities");
	
	if(rtqElement != NULL)
	{
		qualityElement = rtqElement->FirstChildElement("Quality");
		
		while(qualityElement != NULL)
		{
			const string *name = qualityElement->Attribute((const string)"name");	
			
			if(name == NULL)
			{
				cerr << "RealTimeMidiNote::parseXml(): Quality object with no name, skipping\n";
				qualityElement = qualityElement->NextSiblingElement("Quality");
				continue;
			}
			
			if(name->compare("Intensity") == 0)
			{
				//cout << "parsing Intensity\n";
				
				if(intensity_->parseXml(qualityElement))
				{
					cerr << "RealTimeMidiNote::parseXml(): Error parsing Intensity quality.\n";
				}
			}
			else if(name->compare("Brightness") == 0)
			{
				//cout << "parsing Brightness\n";
				
				if(brightness_->parseXml(qualityElement))
				{
					cerr << "RealTimeMidiNote::parseXml(): Error parsing Brightness quality.\n";
				}				
			}
			else if(name->compare("Pitch") == 0)
			{
				//cout << "parsing Pitch\n";
				
				if(pitch_->parseXml(qualityElement))
				{
					cerr << "RealTimeMidiNote::parseXml(): Error parsing Pitch quality.\n";
				}				
			}
			else if(name->compare("Harmonic") == 0)
			{
				//cout << "parsing Pitch\n";
				
				if(harmonic_->parseXml(qualityElement))
				{
					cerr << "RealTimeMidiNote::parseXml(): Error parsing Harmonic quality.\n";
				}				
			}
			else
			{
				cerr << "RealTimeMidiNote::parseXml(): Unknown quality object " << *name << ", skipping\n";
			}
			
			qualityElement = qualityElement->NextSiblingElement("Quality");
		}
		
		qualityElement = rtqElement->FirstChildElement("KeyDownHoldoff");
		
		if(qualityElement != NULL)
		{
			const string *delay = qualityElement->Attribute((const string)"delay");
			const string *scaler = qualityElement->Attribute((const string)"scaler");
			
			if(delay != NULL)
			{
				stringstream delayStream(*delay);
				delayStream >> keyDownHoldoffTime_;	
			}
			if(scaler != NULL)
			{
				stringstream scalerStream(*scaler);
				scalerStream >> keyDownHoldoffScaler_;
			}
			
			useKeyDownHoldoff_ = true;
		}
		
		qualityElement = rtqElement->FirstChildElement("UseHarmonicSweep");
		if(qualityElement != NULL) 
		{
			const string *range = qualityElement->Attribute((const string)"range");
			
			if(range != NULL)
			{
				stringstream rangeStream(*range);
				rangeStream >> harmonicSweepRange_;	
			}
			else 
				harmonicSweepRange_ = 16;
			
			const string *spread = qualityElement->Attribute((const string)"spread");
			
			if(spread != NULL)
			{
				stringstream spreadStream(*spread);
				spreadStream >> harmonicSweepSpread_;	
			}
			else 
				harmonicSweepSpread_ = 0;		
			
			const string *pitchBend = qualityElement->Attribute((const string)"usePitchBend");
			
			if(pitchBend != NULL)
			{
				stringstream s(*pitchBend);
				s >> boolalpha >> usePitchBendWithHarmonics_;				
			}
			else
				usePitchBendWithHarmonics_ = false;
		}
	}
	else
	{
		cerr << "RealTimeMidiNote::parseXml(): no RealTimeQualities element found for Note " << name_ << endl;
	}
	
	return 0;
}

// Just like the superclass, but use a RealTimeMidiNote object instead.  Also set up the initial quality values.

RealTimeMidiNote* RealTimeMidiNote::createNote(int audioChannel, int mrpChannel, int midiNote, int midiChannel, 
											   int pianoString, unsigned int key, int priority, int velocity, 
											   float phaseOffset, float amplitudeOffset,
											   double attack, double intensity, double brightness, double pitch)
{
	RealTimeMidiNote *out = new RealTimeMidiNote(controller_, render_);
	int i;
	float baseFreq;
	
	out->setPerformanceParameters(audioChannel, mrpChannel, midiNote, midiChannel, pianoString, key, priority, velocity);
	
	baseFreq = controller_->midiNoteToFrequency(midiNote);				// Find the center frequency for this note
	out->centerFrequency_ = baseFreq;
	
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
	
	// Copy the qualities from this object (which have been loaded by parseXml) to the newly created note.
	
	out->intensity_ = new PllSynthQuality(*intensity_);
	out->brightness_ = new PllSynthQuality(*brightness_);
	out->pitch_ = new PllSynthQuality(*pitch_);
	out->harmonic_ = new PllSynthQuality(*harmonic_);
	
	out->keyDownHoldoffTime_ = keyDownHoldoffTime_;
	out->keyDownHoldoffScaler_ = keyDownHoldoffScaler_;
	out->useKeyDownHoldoff_ = useKeyDownHoldoff_;
	out->harmonicSweepRange_ = harmonicSweepRange_;
	out->harmonicSweepSpread_ = harmonicSweepSpread_;
	out->usePitchBendWithHarmonics_ = usePitchBendWithHarmonics_;
	
	// TODO: deal with attack
	out->intensity_->setBaseValue(intensity);
	out->intensity_->setVibratoValue(0);
	out->brightness_->setBaseValue(brightness);
	out->brightness_->setVibratoValue(0);
	out->pitch_->setBaseValue(pitch);
	out->pitch_->setVibratoValue(0);
	out->harmonic_->setBaseValue(0);
	out->harmonic_->setVibratoValue(0);
    
//    cout << "----------------- createNote edit ------------------" << endl;
//	
//    // Edit:
//    vector<double> currentHarmonicAmplitudes = getCurrentHarmonicAmplitudes();
//    
//    cout << "Setting harmonic amplitudes to ";
//    for(int i = 0; i < currentHarmonicAmplitudes.size(); ++i)
//    {
//        cout << currentHarmonicAmplitudes[i] << " ";
//    }
//    cout << endl;
//    
//    out->setRawHarmonicValues(currentHarmonicAmplitudes);
//    out->updateSynthParameters();
    
	return out;
}

vector<double> RealTimeMidiNote::getCurrentHarmonicAmplitudes()
{
    vector <double> harmonicAmplitudes;
    
    if(synths_.size() > 0)
    {
        harmonicAmplitudes = ((PllSynth *)synths_[0])->getCurrentHarmonicAmplitudes();
    }
    
    return harmonicAmplitudes;
}

void RealTimeMidiNote::setRawHarmonicValues(vector<double>& values)
{
	usingRawHarmonics_ = true;
	if(synths_.size() > 0)
	{
		vector<timedParameter> vtp;

		((PllSynth *)synths_[0])->setHarmonicAmplitudes(values, vtp);
	}	
}

void RealTimeMidiNote::clearRawHarmonicValues()
{ 
	usingRawHarmonics_ = false;
	updateSynthParameters(); 
}

void RealTimeMidiNote::setAttack(double attack)
{
	// TODO: setAttack
}

void RealTimeMidiNote::updateSynthParameters()
{
	// Here we "render" the internal parameter values of each quantity, combining them into one set that
	// goes to the synth.
	
	// FIXME: Start with more intelligent values?
	
	double startGlobalAmplitude = 1.0, startRelativeFrequency = 1.0, startLoopGain = 1.0;
	double globalAmplitude, relativeFrequency, loopGain;
	bool amplitudeUpdated = false, relativeFrequencyUpdated = false, loopGainUpdated = false, harmonicAmplitudesUpdated = false;
	vector<double> startHarmonicAmplitudes, startHarmonicPhases, harmonicAmplitudes, harmonicPhases;
	int i;
	
	/*for(i = 0; i < 16; i++)		// Start with blank slate instead...
	{
		harmonicAmplitudes.push_back(1.0);
		harmonicPhases.push_back(1.0);
	}*/
	
	globalAmplitude = intensity_->scaleGlobalAmplitude(startGlobalAmplitude, &amplitudeUpdated);
	globalAmplitude = brightness_->scaleGlobalAmplitude(globalAmplitude, &amplitudeUpdated);
	globalAmplitude = pitch_->scaleGlobalAmplitude(globalAmplitude, &amplitudeUpdated);
	globalAmplitude = harmonic_->scaleGlobalAmplitude(globalAmplitude, &amplitudeUpdated);
	
	relativeFrequency = intensity_->scaleRelativeFrequency(startRelativeFrequency, &relativeFrequencyUpdated);
	relativeFrequency = brightness_->scaleRelativeFrequency(relativeFrequency, &relativeFrequencyUpdated);
	relativeFrequency = pitch_->scaleRelativeFrequency(relativeFrequency, &relativeFrequencyUpdated);
	relativeFrequency = harmonic_->scaleRelativeFrequency(relativeFrequency, &relativeFrequencyUpdated);

	loopGain = intensity_->scaleLoopGain(startLoopGain, &loopGainUpdated);
	loopGain = brightness_->scaleLoopGain(loopGain, &loopGainUpdated);
	loopGain = pitch_->scaleLoopGain(loopGain, &loopGainUpdated);
	loopGain = harmonic_->scaleLoopGain(loopGain, &loopGainUpdated);
	
	harmonicAmplitudes = intensity_->scaleHarmonicAmplitudes(startHarmonicAmplitudes, &harmonicAmplitudesUpdated);
	harmonicAmplitudes = brightness_->scaleHarmonicAmplitudes(harmonicAmplitudes, &harmonicAmplitudesUpdated);
	harmonicAmplitudes = pitch_->scaleHarmonicAmplitudes(harmonicAmplitudes, &harmonicAmplitudesUpdated);
	harmonicAmplitudes = harmonic_->scaleHarmonicAmplitudes(harmonicAmplitudes, &harmonicAmplitudesUpdated);
	
	/*harmonicPhases = intensity_->scaleHarmonicPhases(startHarmonicPhases);
	harmonicPhases = brightness_->scaleHarmonicPhases(harmonicPhases);
	harmonicPhases = pitch_->scaleHarmonicPhases(harmonicPhases);
	harmonicPhases = harmonic_->scaleHarmonicPhases(harmonicPhases);*/
	
	// As long as we have at least one synth (which we assume is of type PllSynth) we can update
	// its parameters.  This may be more computational overhead than necessary if nothing has changed
	// since last update on most paramters.
	
	if(synths_.size() > 0)
	{
		timedParameter tp;
		vector<timedParameter> vtp;
		
//		cout << "Setting globalAmplitude to " << globalAmplitude << endl;
//		cout << "Setting centerFrequency to " << relativeFrequency * centerFrequency_ << endl;
//        cout << "Setting loop gain to " << loopGain << endl;
//		for(i = 0; i < harmonicAmplitudes.size(); i++)
//			cout << "Setting harmonicAmplitudes[" << i <<"] to " << harmonicAmplitudes[i] << endl;
		
		if(amplitudeUpdated)
			((PllSynth *)synths_[0])->setGlobalAmplitude(globalAmplitude, tp);
		if(relativeFrequencyUpdated)
			((PllSynth *)synths_[0])->setCenterFrequency(relativeFrequency * centerFrequency_, tp);
		if(loopGainUpdated)
			((PllSynth *)synths_[0])->setLoopGain(loopGain, tp);
		if(!usingRawHarmonics_ && harmonicAmplitudesUpdated)
			((PllSynth *)synths_[0])->setHarmonicAmplitudes(harmonicAmplitudes, vtp);
		//((PllSynth *)synths_[0])->setHarmonicPhases(harmonicPhases, vtp);
	}
}

void RealTimeMidiNote::setAbsolutePitch(double midiNotePitch, double targetPitch, bool scaleIntensity)
{
    // Calculate the relative pitch multipler for the target and current relative pitch
    double targetRelativePitch = 1 + (targetPitch - midiNotePitch) / midiNotePitch;
    double currentRelativePitch = pitch_->getCurrentRelativeFrequency();
    double pitchBaseMultiplier = targetRelativePitch / currentRelativePitch;
    
    // Inverse of the operations performed in RealTimeMidiNote::PllSynthQuality::transeg()
    vector<double> relativePitchRange = pitch_->getRelativeFrequencyRange();
    double logBase = relativePitchRange[1] / relativePitchRange[0];
    double pitchBaseExponent = log(pitchBaseMultiplier / relativePitchRange[0]) / log(logBase);
    
    if (scaleIntensity)
    {
        /* Scale the intensity based on distance of pitch bend from center frequency because actuating a string off its fundamental
         or harmonics results in a weaker sound. */
        double intensity = getCurrentIntensity() + fabs(targetRelativePitch);
        
        if (intensity > 2.0)    intensity = 2.0;
        
        setAbsoluteIntensityBase(intensity);
    }
    setRelativePitchBase(pitchBaseExponent);
}

RealTimeMidiNote::~RealTimeMidiNote()
{
#ifdef DEBUG_ALLOCATION
	cout << "**** ~RealTimeMidiNote\n";
#endif
	
	delete intensity_;
	delete brightness_;
	delete pitch_;
	delete harmonic_;
}


#pragma mark PllSynthQuality

int RealTimeMidiNote::PllSynthQuality::parseXml(TiXmlElement *baseElement)
{
	TiXmlElement *element = baseElement->FirstChildElement("Parameter");
	
	while(element != NULL)
	{
		// Each Parameter holds three attribute tags: name, value, and (optionally) concavity for velocity-sensitive parameters
		
		const string *name = element->Attribute((const string)"name");
		const string *value = element->Attribute((const string)"value");
		const string *concavity = element->Attribute((const string)"concavity");
		const string *mode = element->Attribute((const string)"mode");
		
		if(value != NULL && name != NULL)	
		{
			stringstream valueStream(*value);
			double c = 0.0;
			bool linear = true, absolute = false;
			
//#ifdef DEBUG_MESSAGES_EXTRA
			cout << "PllSynthQuality::parseXml(): Processing name = '" << (*name) << "', value = '" << (*value) << "'\n";
//#endif
			if(concavity != NULL)		// Read the concavity attribute if present; if not, assume linear (0)
			{
				stringstream concavityStream(*concavity);
				concavityStream >> c;
			}
			if(mode != NULL)
			{
				if(mode->compare("exp") == 0 || mode->compare("exponential") == 0)
					linear = false;
				else if(mode->compare("abs") == 0 || mode->compare("absolute") == 0)
					absolute = true;
				else if(mode->compare("exp-abs") == 0 || mode->compare("exponential-abs") == 0 ||
						mode->compare("exp-absolute") == 0 || mode->compare("exponential-absolute") == 0)
				{
					linear = false;
					absolute = true;
				}

			}
			
			// First, check that the name is a parameter we recognize.  Some will have float values, others will be a list of floats.
			// We don't look for a phase offset parameter in the XML file, since that is separately calibrated
			if(name->compare("RelativeFrequency") == 0) {			// double, time-variant
				MidiNote::parseVelocityPair(value->c_str(), &relativeFrequencyMin_, &relativeFrequencyMax_);
				relativeFrequencyConcavity_ = c;
				relativeFrequencyLinear_ = linear;
				relativeFrequencyAbsolute_ = absolute;
				useRelativeFrequency_ = true;
			}
			else if(name->compare("GlobalAmplitude") == 0) {			// double, time-variant
				MidiNote::parseVelocityPair(value->c_str(), &globalAmplitudeMin_, &globalAmplitudeMax_);
				globalAmplitudeConcavity_ = c;
				globalAmplitudeLinear_ = linear;
				globalAmplitudeAbsolute_ = absolute;
				useGlobalAmplitude_ = true;		
			}
			else if(name->compare("HarmonicAmplitudes") == 0) {			// vector<double>, time-variant
				MidiNote::parseCommaSeparatedValuesWithVelocity(value->c_str(), &harmonicAmplitudesMin_, &harmonicAmplitudesMax_);		// Collect the starting values
				harmonicAmplitudesConcavity_ = c;
				harmonicAmplitudesLinear_ = linear;
				harmonicAmplitudesAbsolute_ = absolute;
				useHarmonicAmplitudes_ = true;
			}
			else if(name->compare("HarmonicPhases") == 0) {				// vector<double>, time-variant
				MidiNote::parseCommaSeparatedValuesWithVelocity(value->c_str(), &harmonicPhasesMin_, &harmonicPhasesMax_);		// Collect the starting values
				harmonicPhasesConcavity_ = c;
				harmonicPhasesLinear_ = linear;
				harmonicPhasesAbsolute_ = absolute;
				useHarmonicPhases_ = true;
			}
			else if(name->compare("LoopGain") == 0) {					// double, time-variant
				MidiNote::parseVelocityPair(value->c_str(), &loopGainMin_, &loopGainMax_);
				loopGainConcavity_ = c;
				loopGainLinear_ = linear;
				loopGainAbsolute_ = absolute;
				useLoopGain_ = true;	
			}			
			else if(name->compare("HarmonicCentroid") == 0) {
				// A couple extra parameters for this one
				const string *round = element->Attribute((const string)"round");
				const string *shift = element->Attribute((const string)"shift");
				
				MidiNote::parseVelocityPair(value->c_str(), &harmonicCentroidMin_, &harmonicCentroidMax_);
				harmonicCentroidConcavity_ = c;
				harmonicCentroidLinear_ = linear;
				harmonicCentroidAbsolute_ = absolute;
				if(round == NULL)
					harmonicCentroidRoundMin_ = harmonicCentroidRoundMax_ = 0.0;
				else
					MidiNote::parseVelocityPair(round->c_str(), &harmonicCentroidRoundMin_, &harmonicCentroidRoundMax_);
				if(shift == NULL)
					harmonicCentroidMultiply_ = false;
				else
				{
					if(shift->compare(0, 3, "mul") == 0)
					{
						harmonicCentroidMultiply_ = true;
					}
				}
				useHarmonicCentroid_ = true;
			}
			else
				cerr << "PllSynthQuality::parseXml() warning: unknown parameter '" << *name << "'\n";
		}
		
		element = element->NextSiblingElement("Parameter");
	}
	
	element = baseElement->FirstChildElement("Vibrato");
	
	if(element == NULL)
	{
		vibratoWeight_ = 0.0;
		clipVibratoLower_ = clipVibratoUpper_ = false;
	}
	else
	{
		const string *weight = element->Attribute((const string)"weight");
		const string *clipLower = element->Attribute((const string)"clipLower");
		const string *clipUpper = element->Attribute((const string)"clipUpper");
		
		if(weight != NULL)
		{
			stringstream s(*weight);
			s >> vibratoWeight_;
			
			//cout << "Vibrato weight set to " << vibratoWeight_ << endl;		
		}
		else
			vibratoWeight_ = 0.0;
		if(clipLower != NULL)
		{
			stringstream s(*clipLower);
			s >> boolalpha >> clipVibratoLower_;
			
			//cout << "Vibrato clipLower set to " << clipVibratoLower_ << endl;		
		}		
		else
			clipVibratoLower_ = false;
		if(clipUpper != NULL)
		{
			stringstream s(*clipUpper);
			s >> boolalpha >> clipVibratoUpper_;
			
			//cout << "Vibrato clipUpper set to " << clipVibratoUpper_ << endl;		
		}		
		else
			clipVibratoUpper_ = false;		
		
		// 1.0 means no vibrato since it's the base of an exponent.  < 0.0 makes no sense.
		
		if(vibratoWeight_ == 1.0 || vibratoWeight_ < 0.0)
			vibratoWeight_ = 0.0;
	}
	
	setBaseValue(0.0);
	setVibratoValue(0.0);
	updateParameters();
	
	return 0;
}

// Update the value of this quantity: this in turn updates the current value of each of its
// affected parameters.  No change is made to the synth until the RealTimeMidiNote explicitly
// does so using the scale...() methods below.

void RealTimeMidiNote::PllSynthQuality::setBaseValue(double value)
{
	if(currentBaseValue_ == value)
		return;
	currentBaseValue_ = value;
	
    cout << "Updating parameters" << endl;
	updateParameters();
}

void RealTimeMidiNote::PllSynthQuality::setVibratoValue(double value)
{
	if(currentVibratoValue_ == value)
		return;
	
	currentVibratoValue_ = value;
	
	updateParameters();
}

// The following methods accumulate the effects of each quality on the synth parameters.  They will be
// called sequentially on each Quality by the controlling RealTimeMidiNote.

double RealTimeMidiNote::PllSynthQuality::scaleGlobalAmplitude(double inGlobalAmplitude, bool *updated)
{
	if(updated != NULL)
		*updated = *updated || useGlobalAmplitude_;
	if(!useGlobalAmplitude_)
		return inGlobalAmplitude;
	
	return inGlobalAmplitude*currentGlobalAmplitude_;
}

double RealTimeMidiNote::PllSynthQuality::scaleRelativeFrequency(double inRelativeFrequency, bool *updated)
{
	if(updated != NULL)
		*updated = *updated || useRelativeFrequency_;	
	if(!useRelativeFrequency_)
		return inRelativeFrequency;
	
	return inRelativeFrequency*currentRelativeFrequency_;
}

vector<double> RealTimeMidiNote::PllSynthQuality::scaleHarmonicAmplitudes(vector<double>& inHarmonicAmplitudes, bool *updated)
{
	vector<double> out;
	int i;

	if(updated != NULL)
		*updated = *updated || (useHarmonicAmplitudes_ || useHarmonicCentroid_);
	
	if(!useHarmonicAmplitudes_)
	{
		for(i = 0; i < inHarmonicAmplitudes.size(); i++)
			out.push_back(inHarmonicAmplitudes[i]);
	}
	else
	{
		double h;
		
		// Add new harmonics to the old ones
		
		for(i = 0; i < inHarmonicAmplitudes.size(); i++)
		{
			if(i < currentHarmonicAmplitudes_.size())
			{
				h = inHarmonicAmplitudes[i] + currentHarmonicAmplitudes_[i];
				out.push_back(h < 0.0 ? 0.0 : h);
			}
			else
				out.push_back(inHarmonicAmplitudes[i]);
		}
		while(i < currentHarmonicAmplitudes_.size())
		{
			h = currentHarmonicAmplitudes_[i++];
			out.push_back(h < 0.0 ? 0.0 : h);
		}
	}
	
	if(!useHarmonicCentroid_)
		return out;
	else							// If we use the harmonic centroid parameter, shift all harmonics upward and round to nearest bin
	{
		vector<double> out2;

		for(i = 0; i < out.size(); i++)
		{
			double num, val;
			
			if(harmonicCentroidMultiply_)
				num = (i+1) * currentHarmonicCentroid_;
			else
				num = (i+1) + currentHarmonicCentroid_;	
			val = out[i];
			
			// val will most probably lie between two integers, in which case we assign each of its neighbors a weighted
			// sum.  Keep some sort of limit on how many harmonics can be defined this way, so the system doesn't get
			// too too slow.
			
			if(num == floor(num))	// num is a pure integer
			{
				while(out2.size() < num)
				{
					if(out2.size() >= MAX_CENTROID_HARMONICS)
						break;
					out2.push_back(0.0);
				}
				if(out2.size() > (int)(num-1))
					out2[(int)(num-1)] += val;
			}
			else					// num has a fractional component
			{
				while(out2.size() < ceil(num))
				{
					if(out2.size() >= MAX_CENTROID_HARMONICS)
						break;
					out2.push_back(0.0);
				}
				if(out2.size() > (int)ceil(num)-1)
				{
					out2[(int)floor(num)-1] += val*(ceil(num) - num);	// e.g. 1.8 --> bin 1 (strength .2), bin 2 (strength .8)
					out2[(int)ceil(num)-1] += val*(num - floor(num));
				}
			}
		}
		
		/*cout << "Harmonics: ";				// DEBUG
		for(i = 0; i < out2.size(); i++)
			cout << out2[i] << " ";
		cout << endl;*/

		return out2;
	}
}

vector<double> RealTimeMidiNote::PllSynthQuality::scaleHarmonicPhases(vector<double>& inHarmonicPhases, bool *updated)
{
	if(!useHarmonicPhases_)
		return inHarmonicPhases;
	
	vector<double> out;
	int i;

	if(updated != NULL)
		*updated = *updated || useHarmonicPhases_;	
	if(!useHarmonicAmplitudes_)
	{
		for(i = 0; i < inHarmonicPhases.size(); i++)
			out.push_back(inHarmonicPhases[i]);
	}
	else 
	{
		// Add new phases to the old ones
		
		for(i = 0; i < inHarmonicPhases.size(); i++)
		{
			if(i < currentHarmonicPhases_.size())
				out.push_back(inHarmonicPhases[i] + currentHarmonicPhases_[i]);
			else
				out.push_back(inHarmonicPhases[i]);
		}
		while(i < currentHarmonicPhases_.size())
		{
			out.push_back(currentHarmonicPhases_[i++]);
		}
	}
	return out;	
}

double RealTimeMidiNote::PllSynthQuality::scaleLoopGain(double inLoopGain, bool *updated)
{
	if(updated != NULL)
		*updated = *updated || useLoopGain_;
	if(!useLoopGain_)
		return inLoopGain;
	
	return inLoopGain*currentLoopGain_;
}

void RealTimeMidiNote::PllSynthQuality::resetHarmonicAmplitudes(vector<double> &targetHarmonicAmplitudes)
{
    harmonicAmplitudesMin_ = targetHarmonicAmplitudes;
    harmonicAmplitudesMax_ = targetHarmonicAmplitudes;
}

// Utility methods


void RealTimeMidiNote::PllSynthQuality::updateParameters()
{
    cout << "PllSynthQuality::updateParameters(): ";
    
	double rawValue = currentCombinedValue();		// Get the summed effect of base and vibrato to control parameters
	double value;
	
	// Update any affected parameters
	
	if(useGlobalAmplitude_)
	{
		value = globalAmplitudeAbsolute_ ? fabs(rawValue) : rawValue;
		
		cout << "min " << globalAmplitudeMin_ << " max " << globalAmplitudeMax_ << " c " << globalAmplitudeConcavity_ << " val " << value << endl;
		currentGlobalAmplitude_ = transeg(globalAmplitudeMin_, globalAmplitudeMax_, globalAmplitudeConcavity_, 
										  value, globalAmplitudeLinear_);
        cout << "currentGlobalAmplitude_ = " << currentGlobalAmplitude_ << endl;
	}
	if(useRelativeFrequency_)
	{
		value = relativeFrequencyAbsolute_ ? fabs(rawValue) : rawValue;
		
		cout << "min " << relativeFrequencyMin_ << " max " << relativeFrequencyMax_ << " c " << relativeFrequencyConcavity_ << " val " << value << endl;
		
		currentRelativeFrequency_ = transeg(relativeFrequencyMin_, relativeFrequencyMax_, relativeFrequencyConcavity_, 
											value, relativeFrequencyLinear_);
    
		cout << "currentRelativeFrequency_ " << currentRelativeFrequency_ << endl;
	}
	if(useHarmonicAmplitudes_)
	{
		value = harmonicAmplitudesAbsolute_ ? fabs(rawValue) : rawValue;
		
		int size = min(harmonicAmplitudesMin_.size(), harmonicAmplitudesMax_.size());
		
		while(currentHarmonicAmplitudes_.size() < size)		// Make sure there are enough elements in the current vector
			currentHarmonicAmplitudes_.push_back(0.0);		// so we don't crash in the loop below
		
		for(int j = 0; j < size; j++)
		{
			currentHarmonicAmplitudes_[j] = transeg(harmonicAmplitudesMin_[j], harmonicAmplitudesMax_[j], 
													harmonicAmplitudesConcavity_, value, harmonicAmplitudesLinear_);
		}	
	}
	if(useHarmonicPhases_)
	{
		value = harmonicPhasesAbsolute_ ? fabs(rawValue) : rawValue;
		
		int size = min(harmonicPhasesMin_.size(), harmonicPhasesMax_.size());
		
		while(currentHarmonicPhases_.size() < size)		// Make sure there are enough elements in the current vector
			currentHarmonicPhases_.push_back(0.0);		// so we don't crash in the loop below
		
		for(int j = 0; j < size; j++)
		{
			currentHarmonicPhases_[j] = transeg(harmonicPhasesMin_[j], harmonicPhasesMax_[j], 
												harmonicPhasesConcavity_, value, harmonicPhasesLinear_);
		}			
	}
	if(useHarmonicCentroid_)
	{
		value = harmonicCentroidAbsolute_ ? fabs(rawValue) : rawValue;
		
		double tempCentroid = transeg(harmonicCentroidMin_, harmonicCentroidMax_, harmonicCentroidConcavity_,
									  value, harmonicCentroidLinear_);
		double currentRound = transeg(harmonicCentroidRoundMin_, harmonicCentroidRoundMax_, 0, value, true);
		double centroidInt;
		
		// FIXME: smoother rounding
		centroidInt = round(tempCentroid);
		currentHarmonicCentroid_ = centroidInt + (tempCentroid - centroidInt)*currentRound;
        
        cout << "currentHarmonicCentroid_ " << currentHarmonicCentroid_ << endl;
	}	
	if(useLoopGain_)
	{
		value = loopGainAbsolute_ ? fabs(rawValue) : rawValue;
		
		currentLoopGain_ = transeg(loopGainMin_, loopGainMax_, loopGainConcavity_, fabs(value), loopGainLinear_);
        
        cout << "currentLoopGain_ = " << currentLoopGain_ << endl;
	}	
}

double RealTimeMidiNote::PllSynthQuality::transeg(double outVal1, double outVal2, double concavity, double inVal, bool linear)
{
	// Given a low and high value, a concavity and a velocity, return a normalized value
	// This operates similar to csound's transeg opcode.  concavity > 0 produces a slowly rising (concave) curve
	// where concavity < 0 produces a convex curve.  concavity = 0 produces a linear curve.
	
	if(outVal1 == outVal2) 
		return outVal1;
	
	double temp = inVal;
	
	if(concavity != 0.0)
		temp = (1.0 - exp(inVal * concavity))/(1.0 - exp(concavity));
	if(linear)
	{
		return outVal1 + (outVal2 - outVal1) * temp;
	}
	else
	{
		if(outVal1 == 0.0)
			return 0.0;
		return outVal1 * pow(outVal2 / outVal1, temp);
	}
}
