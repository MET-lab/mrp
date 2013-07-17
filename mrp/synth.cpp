/*
 *  synth.cpp
 *  mrp
 *
 *  Created by Andrew McPherson on 10/16/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "synth.h"
#include "wavetables.h"
#include "config.h"

#define min(x,y) (x<y?x:y)
#define SEMITONE_UP (1.059463094359295)		// pow(2, 1/12)
#define SEMITONE_DOWN (0.943874312681694)	// pow(2, -1/12)

#define MIDDLE_C 261.63	// Hz

extern WaveTable waveTable;

#pragma mark SynthBase

ostream& operator<<(ostream& output, const SynthBase& s)
{
	output << "SynthBase: (address " << (&s) << ")\n";
	output << "  numInputChannels_ = " << s.numInputChannels_ << ", numOutputChannels_ = " << s.numOutputChannels_;
	output << ", outputChannel_ = " << s.outputChannel_ << endl;
	output << "  sampleRate_ = " << s.sampleRate_ << ", sampleNumber_ = " << s.sampleNumber_ << endl;
	if(s.isRunning_)
		output << "  Running | ";
	else
		output << "  Not Running | ";
	if(s.isReleasing_)
		output << "Releasing | ";
	else
		output << "Not Releasing | ";
	output << "startTime = " << s.startTime_ << ", releaseTime_ = " << s.releaseTime_ << endl;
	return output;
}

SynthBase::SynthBase(float sampleRate)
{
	// Copy data to internal variables
	sampleRate_	= sampleRate;
	sampleLength_ = (PaTime) (1.0/(double)sampleRate);

	// At initialization, note is neither running nor releasing
	isRunning_ = isReleasing_ = isFinished_ = false;
	startTime_ = releaseTime_ = (PaTime)0.0;
	numInputChannels_ = numOutputChannels_ = outputChannel_ = 0;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** SynthBase\n";
#endif
}

// This method is called by the controller right before a note is performed, to set the performance-specific
// parameters.  Other parameters of the note might remain more-or-less the same from one MIDI note to the
// next, but we probably won't know these until the last minute.

void SynthBase::setPerformanceParameters(int numInputChannels, int numOutputChannels, int outputChannel)
{
	numInputChannels_ = numInputChannels;
	numOutputChannels_ = numOutputChannels;
	outputChannel_ = outputChannel;
}

// Thoughts: with this system, there is a granularity of attack time equal to the number of frames
// per buffer (at least for notes set to begin "now").  At any given instant, the render thread will
// be calculating audio for some time in the future, so clearly the note should begin as soon as possible.
// On the other hand, the amount of CPU time spent in the render is indeterminate (probably very small),
// so the only predictable behavior is to wait until the beginning of the next render loop to start the note.

// Tell the note to begin playing now
void SynthBase::begin()
{
	isRunning_ = true;
	isReleasing_ = false;
	isFinished_ = false;
	startTime_ = 0.0;
	sampleNumber_ = 0;	
}

// Tell the note to start playing at the specified time
void SynthBase::begin(PaTime when)
{
	isRunning_ = true;
	isReleasing_ = false;
	isFinished_ = false;
	startTime_ = when;
	sampleNumber_ = 0;	
}

// Tell the note to release now, but allow it extra time to handle any extra tail
void SynthBase::release()
{
	isReleasing_ = true;
	releaseTime_ = 0.0;
}

// Tell the note to release at a specified time
void SynthBase::release(PaTime when)
{
	isReleasing_ = true;
	releaseTime_ = when;
}

SynthBase::~SynthBase()
{
	// Nothing to do here, for now.  Possibly remove this synth from the render list.
#ifdef DEBUG_ALLOCATION
	cout << "*** ~SynthBase\n";
#endif
}

#pragma mark PllSynth

ostream& operator<<(ostream& output, const PllSynth& s)
{
	int i;
	
	output << (SynthBase&)s;
	output << "PllSynth subclass:\n";
	output << "  centerFrequency_: " << *(s.centerFrequency_);
	output << "  globalAmplitude_: " << *(s.globalAmplitude_);
	output << "  loopGain_: " << *(s.loopGain_);
	output << "  phaseOffset_: " << *(s.phaseOffset_);
	for(i = 0; i < s.inputGains_.size(); i++)
		output << "  inputGains[" << i << "]: " << *(s.inputGains_[i]);
	for(i = 0; i < s.inputDelays_.size(); i++)
		output << "  inputDelays[" << i << "]: " << *(s.inputDelays_[i]);
	for(i = 0; i < s.harmonicAmplitudes_.size(); i++)
		output << "  harmonicAmplitudes[" << i << "]: " << *(s.harmonicAmplitudes_[i]);
	for(i = 0; i < s.harmonicPhases_.size(); i++)
		output << "  harmonicPhases[" << i << "]: " << *(s.harmonicPhases_[i]);
	output << "  filterQ = " << s.filterQ_ << " ";
	output << "loopFilterPole = " << s.loopFilterPole_ << " ";
	output << "loopFilterZero = " << s.loopFilterZero_;
	output << endl;
	if(s.useAmplitudeFeedback_)
		output << "  Amplitude Feedback | ";
	else
		output << "  No Amplitude Feedback | ";
	if(s.useInterferenceRejection_)
		output << "Interference Rejection | ";
	else
		output << "No Interference Rejection | ";
	if(s.usingDelayAndSum_)
		output << "Delay-and-Sum\n";
	else
		output << "No Delay-and-Sum\n";
	if(s.useAmplitudeFeedback_)
	{
		output << "  amplitudeFeedbackScaler_: " << *(s.amplitudeFeedbackScaler_);
	}
	
	return output;
}

// First constructor allows a generic loop filter specification

PllSynth::PllSynth(float sampleRate) : SynthBase(sampleRate)
{
	float defaultFreq = 440.0;
	
	loopFilter_ = NULL;					// These objects are created later, if necessary
	amplitudeFeedbackScaler_ = NULL;	// It's important they start as NULL so we know they haven't been
	mainEnvelopeFollower_ = NULL;		// initialized yet.
	lowEnvelopeFollower_ = highEnvelopeFollower_ = NULL;
	lowInputFilter_ = highInputFilter_ = NULL;
	useAmplitudeFeedback_ = useInterferenceRejection_ = false;
	
	pllLastOutput_ = 0.0;
	
	// Initialize the mutex
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize mutex in PllSynth\n";
		// Throw exception?
	}	
	
	// Set defaults for changeable parameters, so we don't have to worry about allocating Parameters later
	filterQ_ = 50.0;
	filterQinverse_ = 1.0/filterQ_;
	centerFrequency_ = new Parameter(defaultFreq, sampleRate);
	loopGain_ = new Parameter(0.0, sampleRate);
	loopGainWasZero_ = true;								
	phaseOffset_ = new Parameter(0.0, sampleRate);
	inputGains_.push_back(new Parameter(1.0, sampleRate));		// By default, 1 input with no delay
	inputDelays_.push_back(new Parameter(0.0, sampleRate));
	
	usingDelayAndSum_ = false;
	harmonicAmplitudes_.push_back(new Parameter(1.0, sampleRate)); // By default, 1 sine wave
	harmonicPhases_.push_back(new Parameter(0.0, sampleRate));
	
	// By default, amplitude 0.1 (-20dB) and no filters
	globalAmplitude_ = new Parameter(0.1, sampleRate);
	
	// Initialize main input filter buffer (others are initialized if 
	mainInputFilter_ = new ButterBandpassFilter(sampleRate);
	mainInputFilter_->updateCoefficients(defaultFreq, defaultFreq*filterQinverse_);
	
	// Set the loop filter coefficients to default values
	setLoopFilterPoleZero(1.0, 100.0);
	
#ifdef DEBUG_ALLOCATION
	cout << "*** PllSynth\n";
#endif
}

PllSynth::PllSynth(const PllSynth& copy) : SynthBase(copy)
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** PllSynth (copy constructor)\n";
#endif
	
	// Copy all these parameters over.  First the easy ones.
	
	useAmplitudeFeedback_ = copy.useAmplitudeFeedback_;
	useInterferenceRejection_ = copy.useInterferenceRejection_;
	filterQ_ = copy.filterQ_;
	filterQinverse_ = copy.filterQinverse_;
	loopFilterPole_ = copy.loopFilterPole_;
	loopFilterZero_ = copy.loopFilterZero_;
	pllPhase_ = copy.pllPhase_;
	usingDelayAndSum_ = copy.usingDelayAndSum_;
	loopGainWasZero_ = copy.loopGainWasZero_;
	pllLastOutput_ = copy.pllLastOutput_;
	
	// Initialize a new mutex.  Just because the copy was locked doesn't mean this one should be.

	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize mutex in PllSynth\n";
		// Throw exception?
	}	
	
	// Copy all the pointer objects
	if(copy.centerFrequency_ != NULL)
		centerFrequency_ = new Parameter(*copy.centerFrequency_);
	else
		centerFrequency_ = NULL;
	if(copy.mainInputFilter_ != NULL)
		mainInputFilter_ = new ButterBandpassFilter(*copy.mainInputFilter_);
	else
		mainInputFilter_ = NULL;
	if(copy.lowInputFilter_ != NULL)
		lowInputFilter_ = new ButterBandpassFilter(*copy.lowInputFilter_);
	else
		lowInputFilter_ = NULL;
	if(copy.highInputFilter_ != NULL)
		highInputFilter_ = new ButterBandpassFilter(*copy.highInputFilter_);
	else
		highInputFilter_ = NULL;
	if(copy.mainEnvelopeFollower_ != NULL)
		mainEnvelopeFollower_ = new EnvelopeFollower(*copy.mainEnvelopeFollower_);
	else
		mainEnvelopeFollower_ = NULL;
	if(copy.lowEnvelopeFollower_ != NULL)
		lowEnvelopeFollower_ = new EnvelopeFollower(*copy.lowEnvelopeFollower_);
	else
		lowEnvelopeFollower_ = NULL;
	if(copy.highEnvelopeFollower_ != NULL)
		highEnvelopeFollower_ = new EnvelopeFollower(*copy.highEnvelopeFollower_);
	else
		highEnvelopeFollower_ = NULL;	
	if(copy.loopFilter_ != NULL)
		loopFilter_ = new GenericFilter(*copy.loopFilter_);
	else
		loopFilter_ = NULL;
	if(copy.amplitudeFeedbackScaler_ != NULL)
		amplitudeFeedbackScaler_ = new Parameter(*copy.amplitudeFeedbackScaler_);
	else
		amplitudeFeedbackScaler_ = NULL;
	if(copy.loopGain_ != NULL)
		loopGain_ = new Parameter(*copy.loopGain_);
	else
		loopGain_ = NULL;
	if(copy.phaseOffset_ != NULL)
		phaseOffset_ = new Parameter(*copy.phaseOffset_);
	else
		phaseOffset_ = NULL;
	if(copy.globalAmplitude_ != NULL)
		globalAmplitude_ = new Parameter(*copy.globalAmplitude_);
	else
		globalAmplitude_ = NULL;
	
	// Finally, copy over the vector parameters.  Fortunately, we shouldn't have NULL pointers in the
	// vectors.
	
	for(i = 0; i < copy.inputGains_.size(); i++)
		inputGains_.push_back(new Parameter(*copy.inputGains_[i]));
	for(i = 0; i < copy.inputDelays_.size(); i++)
		inputDelays_.push_back(new Parameter(*copy.inputDelays_[i]));
	for(i = 0; i < copy.harmonicInputFilters_.size(); i++)
		harmonicInputFilters_.push_back(new ButterBandpassFilter(*copy.harmonicInputFilters_[i]));
	for(i = 0; i < copy.harmonicEnvelopeFollowers_.size(); i++)
		harmonicEnvelopeFollowers_.push_back(new EnvelopeFollower(*copy.harmonicEnvelopeFollowers_[i]));
	for(i = 0; i < copy.harmonicAmplitudes_.size(); i++)
		harmonicAmplitudes_.push_back(new Parameter(*copy.harmonicAmplitudes_[i]));
	for(i = 0; i < copy.harmonicPhases_.size(); i++)
		harmonicPhases_.push_back(new Parameter(*copy.harmonicPhases_[i]));
}

// TODO: operator=

void PllSynth::setFilterQ(double filterQ)
{
	pthread_mutex_lock(&parameterMutex_);	
	
	filterQ_ = filterQ;							// Save the new Q value
	filterQinverse_ = 1.0/filterQ;				// Calculate Q inverse to save float divisions later
	
	float freq = centerFrequency_->currentValue();
	float freqDivQ = freq*filterQinverse_;		// Save some multiplies...
	
	if(mainInputFilter_ != NULL)				// Update all the bandpass filters
		mainInputFilter_->updateCoefficients(freq, freqDivQ);
	if(useInterferenceRejection_)
	{
		lowInputFilter_->updateCoefficients(freq*SEMITONE_DOWN, freqDivQ*SEMITONE_DOWN);
		highInputFilter_->updateCoefficients(freq*SEMITONE_UP, freqDivQ*SEMITONE_UP);	
	}
	if(useAmplitudeFeedback_)
	{
		for(int j = 0; j < harmonicInputFilters_.size(); j++)
			harmonicInputFilters_[j]->updateCoefficients(freq*(float)(j+2), freqDivQ*(float)(j+2));
	}	
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PllSynth::setLoopFilterPole(float loopFilterPole)
{
	loopFilterPole_ = loopFilterPole;
	if(loopFilterZero_ >= 0.0)
		setLoopFilterPoleZero(loopFilterPole_, loopFilterZero_);		// Update coefficients
}

void PllSynth::setLoopFilterZero(float loopFilterZero)
{
	loopFilterZero_ = loopFilterZero;
	if(loopFilterPole_ >= 0.0)
		setLoopFilterPoleZero(loopFilterPole_, loopFilterZero_);		// Update coefficients
}

void PllSynth::setLoopFilterPoleZero(float loopFilterPole, float loopFilterZero)
{
	vector<double> a, b;
	
	loopFilterPole_ = loopFilterPole;
	loopFilterZero_ = loopFilterZero;
	
	// Calculate coefficients for loop filter
	// If pole is at the origin, can't have the scalar be 0 or the filter will always return 0
	double poleZeroRatio = (loopFilterPole == 0.0 ? .01 : (double)(loopFilterPole / loopFilterZero));
	double piDivSampleRate = M_PI/(double)sampleRate_;
	
	// Bilinear transform
	b.push_back(poleZeroRatio); // b[0]
	b.push_back(-poleZeroRatio*(1.0 - piDivSampleRate*loopFilterZero)/(1.0 + piDivSampleRate*loopFilterZero)); // b[1]
	a.push_back(-(1.0 - piDivSampleRate*loopFilterPole)/(1.0 + piDivSampleRate*loopFilterPole)); // a[1] (a[0] = 1.0 always)
	
	// Initialize the loop filter
	if(loopFilter_ != NULL)
		delete loopFilter_;
	loopFilter_	= new GenericFilter(a, b);	
}

void PllSynth::setLoopFilterAB(vector<double>& loopFilterA, vector<double>& loopFilterB)
{
	// Initialize the loop filter
	if(loopFilter_ != NULL)
		delete loopFilter_;
	loopFilter_	= new GenericFilter(loopFilterA, loopFilterB);	
}

void PllSynth::setUseAmplitudeFeedback(bool useAmplitudeFeedback)
{
	useAmplitudeFeedback_ = useAmplitudeFeedback;	
	
	if(useAmplitudeFeedback_)
	{
		if(amplitudeFeedbackScaler_ == NULL)
			amplitudeFeedbackScaler_ = new Parameter(4.0, sampleRate_); // Gain constant for amplitude feedback
		
		// For either amplitude feedback or interference rejection, we need this envelope follower.
		// Once it's initialized, nothing further to do with it.
		if(mainEnvelopeFollower_ == NULL)			
			mainEnvelopeFollower_ = new EnvelopeFollower(.05, sampleRate_);

		float freq = centerFrequency_->currentValue();
		float freqDivQ = freq*filterQinverse_;		// Save some multiplies...
		
		// If we have multiple harmonics, we may need to create filters and followers from them too
		while(harmonicAmplitudes_.size() > harmonicInputFilters_.size())
		{
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "setUseAmplitudeFeedback(): adding harmonic input filter\n";
#endif
			ButterBandpassFilter *bp = new ButterBandpassFilter(sampleRate_);
			harmonicInputFilters_.push_back(bp);

#ifdef DEBUG_MESSAGES_EXTRA
			cout << "filter has multiplier " << harmonicInputFilters_.size() << endl;
#endif
			// Use size as a proxy for which harmonic we just added.  Notice that this has to come
			// AFTER the harmonicInputFilters_.push_back() call above, or the calcluation will change.

			bp->updateCoefficients(freq*(float)(harmonicInputFilters_.size()), 
								   freqDivQ*(float)(harmonicInputFilters_.size()));
		}
	
		while(harmonicAmplitudes_.size() > harmonicEnvelopeFollowers_.size())
		{
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "setUseAmplitudeFeedback(): adding harmonic envelope follower\n";
#endif
			EnvelopeFollower *ef = new EnvelopeFollower(.05, sampleRate_);
			harmonicEnvelopeFollowers_.push_back(ef);	
		}
	}
}

void PllSynth::setUseInterferenceRejection(bool useInterferenceRejection)
{
	useInterferenceRejection_ = useInterferenceRejection;	
	
	if(useInterferenceRejection_)
	{
		if(lowInputFilter_ == NULL)
			lowInputFilter_ = new ButterBandpassFilter(sampleRate_);
		if(highInputFilter_ == NULL)
			highInputFilter_ = new ButterBandpassFilter(sampleRate_);

		if(lowEnvelopeFollower_ == NULL)
			lowEnvelopeFollower_ = new EnvelopeFollower(.05, sampleRate_);
		if(highEnvelopeFollower_ == NULL)
			highEnvelopeFollower_ = new EnvelopeFollower(.05, sampleRate_);
		
		// For either amplitude feedback or interference rejection, we need this envelope follower.
		// Once it's initialized, nothing further to do with it.
		if(mainEnvelopeFollower_ == NULL)			
			mainEnvelopeFollower_ = new EnvelopeFollower(.05, sampleRate_);		
		
		float freq = centerFrequency_->currentValue();	// Update the BPF coefficients
		float freqDivQ = freq*filterQinverse_;		
		
		lowInputFilter_->updateCoefficients(freq*SEMITONE_DOWN, freqDivQ*SEMITONE_DOWN);
		highInputFilter_->updateCoefficients(freq*SEMITONE_UP, freqDivQ*SEMITONE_UP);			
	}		
}

// Time-variant parameters

void PllSynth::setInputGains(vector<double>& currentInputGains,
							 vector<timedParameter>& rampInputGains)
{
	// Use the currentInputGains size as our metric.  If rampInputGains.size is greater,
	// ignore the extra.  If it is smaller, don't ramp the last parameters.
	int i, size = currentInputGains.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting amplitude
	while(size > inputGains_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding input gain, size was " << inputGains_.size() << endl;
#endif
		inputGains_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
	{
		if(i < rampInputGains.size())
			inputGains_[i]->setRampValues(currentInputGains[i], rampInputGains[i]);
		else
			inputGains_[i]->setCurrentValue(currentInputGains[i]);
	}
		
	// Activate delay-and-sum code
	if(currentInputGains.size() > 0)
	{
		if(rampInputGains.size() > 0 || currentInputGains.size() > 1 || currentInputGains[0] != 1.0)
		{
#ifdef DEBUG_MESSAGES
			cout << "PllSynth::setInputGains(): using delay and sum\n";
#endif
			usingDelayAndSum_ = true;
		}
	}
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::setInputDelays(vector<double>& currentInputDelays,
							 vector<timedParameter>& rampInputDelays)
{
	// Use the currentInputDelays size as our metric.  If rampInputDelays.size is greater,
	// ignore the extra.  If it is smaller, don't ramp the last parameters.
	int i, size = currentInputDelays.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting delay
	while(size > inputDelays_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding input delay, size was " << inputDelays_.size() << endl;
#endif
		inputDelays_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
	{
		if(i < rampInputDelays.size())
			inputDelays_[i]->setRampValues(currentInputDelays[i], rampInputDelays[i]);
		else
			inputDelays_[i]->setCurrentValue(currentInputDelays[i]);
	}

	if(currentInputDelays.size() > 0)
	{
		if(rampInputDelays.size() > 0 || currentInputDelays.size() > 1 || currentInputDelays[0] != 0.0)
		{
#ifdef DEBUG_MESSAGES
			cout << "PllSynth::setInputDelays(): using delay and sum\n";
#endif
			usingDelayAndSum_ = true;
		}
	}
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::setCenterFrequency(double currentCenterFrequency, timedParameter& rampCenterFrequency) 
{ 
	pthread_mutex_lock(&parameterMutex_);		// Acquire lock ensuring we're not actively rendering

	float freq = currentCenterFrequency;
	float freqDivQ = freq*filterQinverse_;	// Save some multiplies...

	centerFrequency_->setRampValues(freq, rampCenterFrequency);	
	
	if(mainInputFilter_ != NULL)
		mainInputFilter_->updateCoefficients(freq, freqDivQ);
	if(useInterferenceRejection_)
	{
		lowInputFilter_->updateCoefficients(freq*SEMITONE_DOWN, freqDivQ*SEMITONE_DOWN);
		highInputFilter_->updateCoefficients(freq*SEMITONE_UP, freqDivQ*SEMITONE_UP);	
	}
	if(useAmplitudeFeedback_)
	{
		for(int j = 0; j < harmonicInputFilters_.size(); j++)
			harmonicInputFilters_[j]->updateCoefficients(freq*(float)(j+2), freqDivQ*(float)(j+2));
	}		
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::setLoopGain(double currentLoopGain, timedParameter& rampLoopGain) 
{
	pthread_mutex_lock(&parameterMutex_);
	
	loopGain_->setRampValues(currentLoopGain, rampLoopGain);

	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::setPhaseOffset(double currentPhaseOffset, timedParameter& rampPhaseOffset) 
{
	pthread_mutex_lock(&parameterMutex_);	
	
	phaseOffset_->setRampValues(currentPhaseOffset, rampPhaseOffset);
	//cout << "synth: setPhaseOffset " << currentPhaseOffset << endl;
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::setGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	globalAmplitude_->setRampValues(currentAmplitude, rampAmplitude);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

vector<double> PllSynth::getCurrentHarmonicAmplitudes()
{
    vector<double> returnAmplitudes;
    
    for (int i = 0; i < harmonicAmplitudes_.size(); ++i)
    {
        returnAmplitudes.push_back(harmonicAmplitudes_[i]->currentValue());
    }
    
    return returnAmplitudes;
}

void PllSynth::setHarmonicAmplitudes(vector<double>& currentHarmonicAmplitudes,
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
		
		if(useAmplitudeFeedback_)
		{
			// There will always be one fewer filterBuffers than harmonics, since the main buffer is separate	
			
			ButterBandpassFilter *bp = new ButterBandpassFilter(sampleRate_);
			
			EnvelopeFollower *ef = new EnvelopeFollower(.05, sampleRate_);
			harmonicEnvelopeFollowers_.push_back(ef);

			// Use size as a proxy for which harmonic we just added.  Notice that this has to come
			// AFTER the harmonicAmplitudes_.push_back() call above, or the calcluation will change.
			
#ifdef DEBUG_MESSAGES_EXTRA
			cout << "filter has multiplier " << harmonicAmplitudes_.size() << endl;
#endif
			float freq = centerFrequency_->currentValue()*(float)(harmonicAmplitudes_.size());
			bp->updateCoefficients(freq, freq*filterQinverse_);
			harmonicInputFilters_.push_back(bp);
		}
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

void PllSynth::setHarmonicPhases(vector<double>& currentHarmonicPhases,
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

void PllSynth::setAmplitudeFeedbackScaler(double currentScaler, timedParameter& rampScaler) 
{
	if(!useAmplitudeFeedback_)	// This parameter doesn't mean anything without amplitude feedback
		return;
	
	pthread_mutex_lock(&parameterMutex_);
	
	amplitudeFeedbackScaler_->setRampValues(currentScaler, rampScaler);
	
	pthread_mutex_unlock(&parameterMutex_);
}

// FIXME: Should append methods actually increase size??

// Append methods add new ramp values at the end of the current scheduled changes

void PllSynth::appendInputGains(vector<timedParameter>& inputGains)
{
	int i, size = inputGains.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting amplitude
	while(size > inputGains_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding input gain, size was " << inputGains_.size() << endl;
#endif
		inputGains_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		inputGains_[i]->appendRampValues(inputGains[i]);

	// Activate delay-and-sum code
	usingDelayAndSum_ = true;
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::appendInputDelays(vector<timedParameter>& inputDelays)
{
	int i, size = inputDelays.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0 as the default starting delay
	while(size > inputDelays_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding input delay, size was " << inputDelays_.size() << endl;
#endif
		inputDelays_.push_back(newParam);
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		inputDelays_[i]->appendRampValues(inputDelays[i]);
	
	// Activate delay-and-sum code
	usingDelayAndSum_ = true;
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::appendCenterFrequency(timedParameter& centerFrequency) 
{ 
	pthread_mutex_lock(&parameterMutex_);
	
	centerFrequency_->appendRampValues(centerFrequency); 

	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::appendLoopGain(timedParameter& loopGain) 
{
	pthread_mutex_lock(&parameterMutex_);
	
	loopGain_->appendRampValues(loopGain);

	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::appendPhaseOffset(timedParameter& phaseOffset) 
{
	pthread_mutex_lock(&parameterMutex_);
	
	phaseOffset_->appendRampValues(phaseOffset);
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::appendGlobalAmplitude(timedParameter& amplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	globalAmplitude_->appendRampValues(amplitude);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void PllSynth::appendHarmonicAmplitudes(vector<timedParameter>& harmonicAmplitudes) 
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
		
		if(useAmplitudeFeedback_)
		{
			// There will always be one fewer filterBuffers than harmonics, since the main buffer is separate	
			
			ButterBandpassFilter *bp = new ButterBandpassFilter(sampleRate_);
			
			// Use size as a proxy for which harmonic we just added.  Notice that this has to come
			// AFTER the push_back() call above, or the calcluation will change.
			
			float freq = centerFrequency_->currentValue()*(float)(harmonicAmplitudes_.size());
			bp->updateCoefficients(freq, freq*filterQinverse_);
			harmonicInputFilters_.push_back(bp);
		}		
	}
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		harmonicAmplitudes_[i]->appendRampValues(harmonicAmplitudes[i]);
	
	pthread_mutex_unlock(&parameterMutex_);
}

void PllSynth::appendHarmonicPhases(vector<timedParameter>& harmonicPhases) 
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

void PllSynth::appendAmplitudeFeedbackScaler(timedParameter& scaler) 
{
	if(!useAmplitudeFeedback_)	// This parameter doesn't mean anything without amplitude feedback
		return;
	
	pthread_mutex_lock(&parameterMutex_);
	
	amplitudeFeedbackScaler_->appendRampValues(scaler);
	
	pthread_mutex_unlock(&parameterMutex_);
}

// Render one buffer of output.  input holds the incoming audio data.  output may already contain
// audio, so we add our result to it rather than replacing.

int PllSynth::render(const void *input, void *output, unsigned long frameCount,
					 const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags)
{
	float *inBuffer = (float *)input;		// These start by pointing at the beginning of the buffer
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
	
	// If the note has been set to release, check whether that should happen immediately, within this
	// buffer, or later.  If later, then go about our business like normal.
	if(isReleasing_)
	{
		if(releaseTime_ <= bufferStartTime)		// Release immediately
		{
			lastFrame = 0;
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			isFinished_ = true;
#ifdef DEBUG_MESSAGES
			cout << "Releasing immediately" << endl;
#endif
		}
		else if(releaseTime_ < bufferEndTime)		// Release mid-buffer
		{
			lastFrame = (unsigned long)((releaseTime_ - bufferStartTime)*sampleRate_);
#ifdef DEBUG_MESSAGES
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
		float inputSample;		// Delay-and-sum on multiple inputs (partially implemented right now)
		float vcoFrequency;
		float followerMain;
		
		// Handle ramped parameter updates, but not every sample to save CPU time.
		// A parameter needs ramping when there is at least one item in its timedParameter deque.
		if(sampleNumber_ % PARAMETER_UPDATE_INTERVAL == 0)
		{
			pthread_mutex_lock(&parameterMutex_);	// Acquire lock so the parameters don't change

			globalAmplitude_->ramp(PARAMETER_UPDATE_INTERVAL);
			loopGain_->ramp(PARAMETER_UPDATE_INTERVAL);
			phaseOffset_->ramp(PARAMETER_UPDATE_INTERVAL);
			
			if(centerFrequency_->ramp(PARAMETER_UPDATE_INTERVAL) && loopGain_->currentValue() != 0.0)
			{
				// If centerFrequency_ changes, need to update the filter buffer coefficients
				float freq = centerFrequency_->currentValue();
				float freqDivQ = freq*filterQinverse_;	// Save some multiplies...
				
				mainInputFilter_->updateCoefficients(freq, freqDivQ);
				if(useInterferenceRejection_)
				{
					lowInputFilter_->updateCoefficients(freq*SEMITONE_DOWN, freqDivQ*SEMITONE_DOWN);
					highInputFilter_->updateCoefficients(freq*SEMITONE_UP, freqDivQ*SEMITONE_UP);	
				}
				if(useAmplitudeFeedback_)
				{
					for(j = 0; j < harmonicInputFilters_.size(); j++)
						harmonicInputFilters_[j]->updateCoefficients(freq*(float)(j+2), freqDivQ*(float)(j+2));
				}				
			}

			// Should we somehow condense these so we don't have to call so many ramp functions?
			for(it = inputGains_.begin(); it != inputGains_.end(); it++)
				(*it)->ramp(PARAMETER_UPDATE_INTERVAL);
			for(it = inputDelays_.begin(); it != inputDelays_.end(); it++)
				(*it)->ramp(PARAMETER_UPDATE_INTERVAL);
			for(it = harmonicAmplitudes_.begin(); it != harmonicAmplitudes_.end(); it++)
				(*it)->ramp(PARAMETER_UPDATE_INTERVAL);			
			for(it = harmonicPhases_.begin(); it != harmonicPhases_.end(); it++)
				(*it)->ramp(PARAMETER_UPDATE_INTERVAL);
			
			if(useAmplitudeFeedback_)
				amplitudeFeedbackScaler_->ramp(PARAMETER_UPDATE_INTERVAL);
			
			pthread_mutex_unlock(&parameterMutex_);
		}	
		
		if(loopGain_->currentValue() != 0.0)
		{
			if(loopGainWasZero_)	// Have to go back and update the stuff we skipped over before
			{
				float freq = centerFrequency_->currentValue();
				float freqDivQ = freq*filterQinverse_;	// Save some multiplies...
				
				mainInputFilter_->clearBuffer();
				mainInputFilter_->updateCoefficients(freq, freqDivQ);

				if(useInterferenceRejection_)
				{
					lowInputFilter_->clearBuffer();
					highInputFilter_->clearBuffer();
					lowInputFilter_->updateCoefficients(freq*SEMITONE_DOWN, freqDivQ*SEMITONE_DOWN);
					highInputFilter_->updateCoefficients(freq*SEMITONE_UP, freqDivQ*SEMITONE_UP);	
				}				
				if(useAmplitudeFeedback_)
				{
					for(j = 0; j < harmonicInputFilters_.size(); j++)
					{
						harmonicInputFilters_[j]->clearBuffer();
						harmonicInputFilters_[j]->updateCoefficients(freq*(float)(j+2), freqDivQ*(float)(j+2));
					}
				}
				
				loopGainWasZero_ = false;
				pllLastOutput_ = 0.0;
			}
			
			if(numInputChannels_ > 0 && inBuffer != NULL)
			{
				if(!usingDelayAndSum_)	// Bypass the delay and sum code if we don't need it.
					inputSample = *inBuffer;	// Channel 1, gain 1.0
				else
				{
					inputSample = 0.0;
					
					for(j = 0; j < min(inputGains_.size(), numInputChannels_); j++)
					{
						// FIXME: inputDelays not yet implemented.  Need to keep long storage buffers
						// for any channels that are delayed.
						
						inputSample += inBuffer[j]*inputGains_[j]->currentValue();
					}
				}
			}
			
			float currentLoopGain = loopGain_->currentValue();
			
			// 2nd order bandpass filter on input, based at centerFrequency_
			float inputFilteredCenter = mainInputFilter_->filter(inputSample);
			if(useInterferenceRejection_ || useAmplitudeFeedback_)
				followerMain = mainEnvelopeFollower_->filter(inputFilteredCenter);			
			if(useInterferenceRejection_)
			{
				// Interference rejection scales the loop gain down when there's more energy at adjacent
				// semitones than at the desired frequency.  This avoids the problem of PLL locking to the
				// wrong frequency, which results in a note that doesn't play properly.
				
				float inputFilteredLow = lowInputFilter_->filter(inputSample);
				float inputFilteredHigh = highInputFilter_->filter(inputSample);
				
				float followerLow = lowEnvelopeFollower_->filter(inputFilteredLow);
				float followerHigh = highEnvelopeFollower_->filter(inputFilteredHigh);
				
				// Find the maximum interfering signal, then compare it to the target signal
				float followerNeighborMax = max(followerLow, followerHigh);
				
				if(followerNeighborMax > 0.0)
				{
					float ratio = followerMain / followerNeighborMax;
					if(ratio <= 1.0)	// Reduce by ratio^4 (steep fall-off for large interference)
					{
						float ratioSquared = ratio*ratio;
						currentLoopGain *= ratioSquared*ratioSquared;
					}
				}
#ifdef DEBUG_MESSAGES_EXTRA
				if(sampleNumber_ % DEBUG_MESSAGE_SAMPLE_INTERVAL == 0)
				{
					cout << "filters: main = " << inputFilteredCenter << "low = " << inputFilteredLow << "high = " << inputFilteredHigh << endl;
					cout << "followers: main = " << followerMain << " low = " << followerLow << " high = " << followerHigh << endl;
				}				
				
#endif
			}
			
			// PLL loop filter: the input to the PLL is a multiplier block of its last output sample
			// and the current input (which comes from the main bandpass filter), which then goes
			// through the loop filter
			
			float loopFilterOutput = (float)loopFilter_->filter((double)(pllLastOutput_*inputFilteredCenter));

			// Should loopGain be scaled according to centerFrequency?  Currently the relative frequency
			// displacement is smaller at higher frequencies, but maybe we only care about absolute displacement.
			
			vcoFrequency = centerFrequency_->currentValue() + currentLoopGain*loopFilterOutput;

			// Update the phase information
			pllPhase_ = fmod(pllPhase_ + vcoFrequency*sampleLength_, 1.0);		
			
			// Calculate the PLL VCO output (a single sine wave without any of the harmonic or phase
			// offset information that we ultimately send to the DAC).
			pllLastOutput_ = waveTable.lookupInterp(waveTableSine, pllPhase_);	
		}
		else
		{
			// If the loop gain is zero, don't need any of the fancy BPF, PLL processing, since the
			// output will remain at centerFrequency no matter what.  However, we should remember to
			// still pass some data through the filter buffers so we're not left with artifacts later.
			
			loopGainWasZero_ = true;
			
			if(useAmplitudeFeedback_)
			{
				if(!usingDelayAndSum_)	// Bypass the delay and sum code if we don't need it.
					inputSample = *inBuffer;	// Channel 1, gain 1.0
				else
				{
					inputSample = 0.0;
					
					for(j = 0; j < min(inputGains_.size(), numInputChannels_); j++)
					{
						// FIXME: inputDelays not yet implemented.  Need to keep long storage buffers
						// for any channels that are delayed.
						
						inputSample += inBuffer[j]*inputGains_[j]->currentValue();
					}
				}	
			}
			
			vcoFrequency = centerFrequency_->currentValue();
			pllPhase_ = fmod(pllPhase_ + vcoFrequency*sampleLength_, 1.0);
		}
		
		// Calculate the output as a sum of sine waves at each harmonic
		outSample = 0.0;
		float phaseShift = phaseOffset_->currentValue();
		
		if(useAmplitudeFeedback_)
		{
			// Amplitude feedback records the intensity of each harmonic by filtering the input
			// signal.  harmonicAmplitudes holds the target values, and the outputs are adjusted
			// to match this.  Note that we can only make the output amplitudes larger; if they're
			// already too large, just turn off that particular harmonic and wait for it to come down.
			// Not a perfect feedback strategy by any means, but it produces musical results.
			
			int size = min(harmonicInputFilters_.size(), harmonicAmplitudes_.size()-1); // sanity check
			
			// If there are N harmonics (i.e. N amplitudes), there will be N-1 elements in harmonicInputFilters.
			// This is because the fundamental frequency has an amplitude, but its input filter is already
			// handled in inputFilteredCenter followerMain.
			
			// step 1: compare followerMain to harmonicAmplitudes[0]
			// step 2: compare followerHarmonic[n] to harmonicAmplitudes[n+1]
			// procedure: take max(harmonicAmplitude - followerAmplitude, 0)
			//            filter this through a first-order lowpass, to pull out any weird peaks from follower
			//            (is the above necessary?)
			//            scale by some constant-- overall we need to figure out the proper dynamics of this loop
			
			float target = globalAmplitude_->currentValue()*harmonicAmplitudes_[0]->currentValue();
			float outputLevel, hPhase;
			if(target != 0.0)
			{
				outputLevel = amplitudeFeedbackScaler_->currentValue()*max(target - followerMain, (float)0.0);
				hPhase = (harmonicPhases_.size() > 0 ? harmonicPhases_[0]->currentValue() : 0.0);
				outSample += outputLevel*waveTable.lookupInterp(waveTableSine, pllPhase_+hPhase+phaseShift);
				
				if(sampleNumber_ % DEBUG_MESSAGE_SAMPLE_INTERVAL == 0)
				{
					//cout << "Fundamental: target = " << target << " follower = " << followerMain << " output = " << outputLevel << endl;
				}
			}
			
			for(j = 0; j < size; j++)
			{
				target = globalAmplitude_->currentValue()*harmonicAmplitudes_[j+1]->currentValue();
				if(target == 0.0)
					continue;
				
				float filteredHarmonic = harmonicInputFilters_[j]->filter(inputSample);
				float followerHarmonic = harmonicEnvelopeFollowers_[j]->filter(filteredHarmonic);
				
				outputLevel = amplitudeFeedbackScaler_->currentValue()*max(target - followerHarmonic, (float)0.0);
				// TODO: filter this level?
				
				if(sampleNumber_ % DEBUG_MESSAGE_SAMPLE_INTERVAL == 0)
				{
					cout << "Harmonic " << j+1 << ": target = " << target << " follower = " << followerHarmonic << " output = " << outputLevel << endl;
				}
				
				hPhase = (j < harmonicPhases_.size() ? harmonicPhases_[j]->currentValue() : 0.0);				
				outSample += outputLevel*waveTable.lookupInterp(waveTableSine, pllPhase_*(float)(j+1)+hPhase+phaseShift);				
			}			
		}
		else
		{
			// With no amplitude feedback, harmonic amplitudes refer directly to the strength of each
			// partial at the output
			
			for(j = 0; j < harmonicAmplitudes_.size(); j++)
			{
				if(harmonicAmplitudes_[j]->currentValue() == 0.0)
					continue;
			
				float hPhase = (j < harmonicPhases_.size() ? harmonicPhases_[j]->currentValue() : 0.0);
			
				outSample += (float)(harmonicAmplitudes_[j]->currentValue())
							* waveTable.lookupInterp(waveTableSine, pllPhase_*(float)(j+1)+hPhase+phaseShift);
				//outSample += (float)(harmonicAmplitudes_[j]->currentValue())
				//				* waveTable.lookup(waveTableSine, pllPhase_*(float)(j+1)+hPhase);
			}
		}
		
		// Mix the output into the buffer, scaling by the global amplitude
		outBuffer[outputChannel_] += outSample*globalAmplitude_->currentValue();
		
#ifdef DEBUG_MESSAGES_EXTRA
		if(sampleNumber_ % DEBUG_MESSAGE_SAMPLE_INTERVAL == 0)
		{
			cout << "freq = " << vcoFrequency << ", inputLevel = " << followerMain << endl;
		}
#endif
		
		// Update counters for next cycle
		sampleNumber_++;
		inBuffer += numInputChannels_;
		outBuffer += numOutputChannels_;
	}
	
	if(willFinishAtEnd)
		isFinished_ = true;
	
	return paContinue;
}

PllSynth::~PllSynth()
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** ~PllSynth\n";
#endif
	
	// Parameters are dynamically-allocated and need to be deleted:
	
	delete centerFrequency_;
	delete loopGain_;
	delete phaseOffset_;
	
	for(i = 0; i < harmonicAmplitudes_.size(); i++)
		delete harmonicAmplitudes_[i];
	for(i = 0; i < harmonicPhases_.size(); i++)
		delete harmonicPhases_[i];
	for(i = 0; i < inputGains_.size(); i++)
		delete inputGains_[i];
	for(i = 0; i < inputDelays_.size(); i++)
		delete inputDelays_[i];
	
	delete globalAmplitude_;
	delete loopFilter_;
	delete mainInputFilter_;
	
	if(useInterferenceRejection_ || useAmplitudeFeedback_)
		delete mainEnvelopeFollower_;
	if(useInterferenceRejection_)
	{
		delete lowInputFilter_;
		delete highInputFilter_;
		delete lowEnvelopeFollower_;
		delete highEnvelopeFollower_;
	}

	for(i = 0; i < harmonicInputFilters_.size(); i++)
		delete harmonicInputFilters_[i];
	for(i = 0; i < harmonicEnvelopeFollowers_.size(); i++)
		delete harmonicEnvelopeFollowers_[i];
	if(useAmplitudeFeedback_)
		delete amplitudeFeedbackScaler_;
	
	pthread_mutex_destroy(&parameterMutex_);
}

#pragma mark NoiseSynth

ostream& operator<<(ostream& output, const NoiseSynth& s)
{
	int i;
	
	output << (SynthBase&)s;
	output << "NoiseSynth subclass:\n";
	output << "  globalAmplitude_: " << *(s.globalAmplitude_);
	for(i = 0; i < s.filterFrequencies_.size(); i++)
		output << "  filterFrequencies[" << i << "]: " << *(s.filterFrequencies_[i]);
	for(i = 0; i < s.filterAmplitudes_.size(); i++)
		output << "  filterAmplitudes[" << i << "]: " << *(s.filterAmplitudes_[i]);
	for(i = 0; i < s.filterQs_.size(); i++)
		output << "  filterQs[" << i << "]: " << *(s.filterQs_[i]);	
	return output;	
}

NoiseSynth::NoiseSynth(float sampleRate) : SynthBase(sampleRate)
{
#ifdef DEBUG_ALLOCATION
	cout << "*** NoiseSynth\n";
#endif
	
	// Initialize the mutex
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize mutex in NoiseSynth\n";
		// Throw exception?
	}
	
	// Seed the random number generator
	sranddev();
	
	// By default, amplitude 0.1 (-20dB) and no filters
	globalAmplitude_ = new Parameter(0.1, sampleRate);
}

NoiseSynth::NoiseSynth(const NoiseSynth& copy) : SynthBase(copy)
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** NoiseSynth (copy constructor)\n";
#endif
	
	// Make copies of the relevant parameters
	
	if(copy.globalAmplitude_ != NULL)
		globalAmplitude_ = new Parameter(*copy.globalAmplitude_);
	else
		globalAmplitude_ = NULL;
	
	// Copy over the vector parameters.  Fortunately, we shouldn't have NULL pointers in the vectors.
	
	for(i = 0; i < copy.filterFrequencies_.size(); i++)
		filterFrequencies_.push_back(new Parameter(*copy.filterFrequencies_[i]));
	for(i = 0; i < copy.filterQs_.size(); i++)
		filterQs_.push_back(new Parameter(*copy.filterQs_[i]));
	for(i = 0; i < copy.filterAmplitudes_.size(); i++)
		filterAmplitudes_.push_back(new Parameter(*copy.filterAmplitudes_[i]));	
	for(i = 0; i < copy.filters_.size(); i++)
		filters_.push_back(new ButterBandpassFilter(*copy.filters_[i]));
	
	// Initialize (don't copy) the mutex
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize mutex in NoiseSynth\n";
		// Throw exception?
	}
	
	// Seed the random number generator
	sranddev();	
}

// TODO: operator=

void NoiseSynth::setGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	globalAmplitude_->setRampValues(currentAmplitude, rampAmplitude);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::setFilterFrequencies(vector<double>& currentFrequencies, 
									  vector<timedParameter>& rampFrequencies)
{
	// If the argument sizes don't match, use the smaller one.  (They should always match)
	int i, size = min(currentFrequencies.size(), rampFrequencies.size());
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 1kHz as the default starting frequency
	while(size > filterFrequencies_.size())
	{
		Parameter *newParam = new Parameter(1000.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding filter frequency, size was " << filterFrequencies_.size() << endl;
#endif
		filterFrequencies_.push_back(newParam);
	}
	
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		filterFrequencies_[i]->setRampValues(currentFrequencies[i], rampFrequencies[i]);
	
	updateFilters(); // Make new ButterBandpassFilters, if necessary
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::setFilterQs(vector<double>& currentQs, 
									  vector<timedParameter>& rampQs)
{
	// If the argument sizes don't match, use the smaller one.  (They should always match)
	int i, size = min(currentQs.size(), rampQs.size());
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 10 as the default starting Q
	while(size > filterQs_.size())
	{
		Parameter *newParam = new Parameter(10.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding filter Q, size was " << filterQs_.size() << endl;
#endif
		filterQs_.push_back(newParam);
	}
	
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		filterQs_[i]->setRampValues(currentQs[i], rampQs[i]);
	
	updateFilters(); // Make new ButterBandpassFilters, if necessary
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::setFilterAmplitudes(vector<double>& currentAmplitudes, 
							 vector<timedParameter>& rampAmplitudes)
{
	// If the argument sizes don't match, use the smaller one.  (They should always match)
	int i, size = min(currentAmplitudes.size(), rampAmplitudes.size());
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0.0 as the default starting amplitude
	while(size > filterAmplitudes_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding filter amplitude, size was " << filterAmplitudes_.size() << endl;
#endif
		filterAmplitudes_.push_back(newParam);
	}
	
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		filterAmplitudes_[i]->setRampValues(currentAmplitudes[i], rampAmplitudes[i]);
	
	updateFilters(); // Make new ButterBandpassFilters, if necessary
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::appendGlobalAmplitude(timedParameter& amplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	globalAmplitude_->appendRampValues(amplitude);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::appendFilterFrequencies(vector<timedParameter>& frequencies)
{
	// If the argument sizes don't match, use the smaller one.  (They should always match)
	int i, size = frequencies.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 1kHz as the default starting frequency
	while(size > filterFrequencies_.size())
	{
		Parameter *newParam = new Parameter(1000.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding filter frequency, size was " << filterFrequencies_.size() << endl;
#endif
		filterFrequencies_.push_back(newParam);
	}
	
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		filterFrequencies_[i]->appendRampValues(frequencies[i]);
	
	updateFilters(); // Make new ButterBandpassFilters, if necessary
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::appendFilterQs(vector<timedParameter>& qs)
{
	// If the argument sizes don't match, use the smaller one.  (They should always match)
	int i, size = qs.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 10 as the default starting Q
	while(size > filterQs_.size())
	{
		Parameter *newParam = new Parameter(10.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding filter Q, size was " << filterQs_.size() << endl;
#endif
		filterQs_.push_back(newParam);
	}
	
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		filterQs_[i]->appendRampValues(qs[i]);
	
	updateFilters(); // Make new ButterBandpassFilters, if necessary
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void NoiseSynth::appendFilterAmplitudes(vector<timedParameter>& amplitudes)
{
	// If the argument sizes don't match, use the smaller one.  (They should always match)
	int i, size = amplitudes.size();
	
	pthread_mutex_lock(&parameterMutex_);		
	
	// Check if the vector we're appending has more elements than our internal storage, and
	// if so, increase our storage accordingly.  Use 0.0 as the default starting amplitude
	while(size > filterAmplitudes_.size())
	{
		Parameter *newParam = new Parameter(0.0, sampleRate_);
		
#ifdef DEBUG_MESSAGES_EXTRA
		cout << "Adding filter amplitude, size was " << filterAmplitudes_.size() << endl;
#endif
		filterAmplitudes_.push_back(newParam);
	}
	
	// Append the new values to each timedParameter in our internal vector
	for(i = 0; i < size; i++)
		filterAmplitudes_[i]->appendRampValues(amplitudes[i]);
	
	updateFilters(); // Make new ButterBandpassFilters, if necessary
	
	pthread_mutex_unlock(&parameterMutex_);	
}

// Private method called by the set and append methods.  This ensures there
// are enough bandpass filter objects, and updates their values.  The number of
// filters is equal to min(size(freqs), size(qs), size(amplitudes))

void NoiseSynth::updateFilters()
{
	int i;
	int minSize = min(min(filterFrequencies_.size(), filterQs_.size()), filterAmplitudes_.size());
	
	if(minSize == filters_.size())	// Nothing to do here, filters has correct size
		return;
	else if(minSize > filters_.size())	// Need to add new filters
	{
		while((i = filters_.size()) < minSize)
		{
#ifdef DEBUG_MESSAGES
			cout << "NoiseSynth: adding ButterBandpassFilter, size was " << i << endl;
#endif
			ButterBandpassFilter *bp = new ButterBandpassFilter(sampleRate_);
			bp->updateCoefficients(filterFrequencies_[i]->currentValue(),
								   (filterFrequencies_[i]->currentValue())/(filterQs_[i]->currentValue()));
			filters_.push_back(bp);			
		}
	}
	else	// Uh-oh, filters has too many elements.  This will cause trouble in render() so fix ASAP!
	{
		cerr << "Warning: NoiseSynth had " << filters_.size() << " filters and " << minSize << " parameters.\n";
		
		while(filters_.size() > minSize)
			filters_.pop_back();
	}
}

// Render one buffer of output.  input holds the incoming audio data.  output may already contain
// audio, so we add our result to it rather than replacing.

int NoiseSynth::render(const void *input, void *output, unsigned long frameCount,
					 const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags)
{
	float *outBuffer = (float *)output;
	float outSample;
	PaTime bufferStartTime, bufferEndTime;
	unsigned long lastFrame = frameCount;
	unsigned long i, j;
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
	
	// If the note has been set to release, check whether that should happen immediately, within this
	// buffer, or later.  If later, then go about our business like normal.
	if(isReleasing_)
	{
		if(releaseTime_ <= bufferStartTime)		// Release immediately
		{
			lastFrame = 0;
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			isFinished_ = true;
#ifdef DEBUG_MESSAGES
			cout << "NoiseSynth releasing immediately" << endl;
#endif
		}
		else if(releaseTime_ < bufferEndTime)		// Release mid-buffer
		{
			lastFrame = (unsigned long)((releaseTime_ - bufferStartTime)*sampleRate_);
#ifdef DEBUG_MESSAGES
			cout << "NoiseSynth releasing at sample " << lastFrame << endl;
#endif
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			willFinishAtEnd = true;				// Set the finished flag *after* rendering
		}
		// else do nothing
	}
	
	for(i = 0; i < lastFrame; i++)
	{
		// Handle ramped parameter updates, but not every sample to save CPU time.
		// A parameter needs ramping when there is at least one item in its timedParameter deque.
		if(sampleNumber_ % PARAMETER_UPDATE_INTERVAL == 0)
		{
			pthread_mutex_lock(&parameterMutex_);	// Acquire lock so the parameters don't change
			
			globalAmplitude_->ramp(PARAMETER_UPDATE_INTERVAL);

			// Note: there must never be more filters than collections of (freq, Q, amplitude)
			// The set and append methods are responsible for enforcing this
			for(j = 0; j < filters_.size(); j++)
			{
				bool changed;
				
				filterAmplitudes_[j]->ramp(PARAMETER_UPDATE_INTERVAL);
				changed = filterFrequencies_[j]->ramp(PARAMETER_UPDATE_INTERVAL) || filterQs_[j]->ramp(PARAMETER_UPDATE_INTERVAL);

				if(changed)
				{
					filters_[j]->updateCoefficients(filterFrequencies_[j]->currentValue(), 
													(filterFrequencies_[j]->currentValue())/(filterQs_[j]->currentValue()));
				}
			}
			
			pthread_mutex_unlock(&parameterMutex_);
		}	

		// Start with a noise source between -1 and 1
		float noise = (float)rand()/(float)(RAND_MAX>>1) - 1.0;
		
		if(filters_.size() == 0)	// With no filters, pass the noise through
			outSample = noise;
		else						// Output sample is a sum of all filter outputs
		{
			outSample = 0.0;
			
			for(j = 0; j < filters_.size(); j++)	// Multiply the output by the Q to keep total energy the same
				outSample += filters_[j]->filter(noise)*filterAmplitudes_[j]->currentValue()*filterQs_[j]->currentValue();
		}
		outSample *= globalAmplitude_->currentValue();	// Scale by overall output level
		
		// Mix the output into the buffer
		outBuffer[outputChannel_] += outSample;
		
		// Update counters for next cycle
		outBuffer += numOutputChannels_;
		sampleNumber_++;
	}
	
	if(willFinishAtEnd)
		isFinished_ = true;
	
	return paContinue;
}


NoiseSynth::~NoiseSynth()
{
	int i;
	
#ifdef DEBUG_ALLOCATION
	cout << "*** ~NoiseSynth\n";
#endif
	for(i = 0; i < filterAmplitudes_.size(); i++)
		delete filterAmplitudes_[i];
	for(i = 0; i < filterQs_.size(); i++)
		delete filterQs_[i];
	for(i = 0; i < filterFrequencies_.size(); i++)
		delete filterFrequencies_[i];
	for(i = 0; i < filters_.size(); i++)
		delete filters_[i];
	delete globalAmplitude_;
	
	pthread_mutex_destroy(&parameterMutex_);	
}

#pragma mark ResonanceSynth

ResonanceSynth::ResonanceSynth(float sampleRate) : SynthBase(sampleRate)
{
#ifdef DEBUG_ALLOCATION
	cout << "*** ResonanceSynth\n";
#endif
	
	// Initialize the mutexes
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize parameter mutex in ResonanceSynth\n";
		// Throw exception?
	}
	if(pthread_mutex_init(&noteMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize note mutex in ResonanceSynth\n";
		// Throw exception?
	}	
	
	// By default, amplitude 0.1 (-20dB) and 6dB / octave harmonic rolloff, .5 sec decay at middle C
	globalAmplitude_ = new Parameter(0.1, sampleRate);
	harmonicRolloff_ = new Parameter(0.5, sampleRate);
	decayRate_ = new Parameter(0.5, sampleRate);
	mono_ = false;
}

ResonanceSynth::ResonanceSynth(const ResonanceSynth& copy) : SynthBase(copy)
{
#ifdef DEBUG_ALLOCATION
	cout << "*** ResonanceSynth (copy constructor)\n";
#endif
	
	// Make copies of the relevant parameters
	
	if(copy.globalAmplitude_ != NULL)
		globalAmplitude_ = new Parameter(*copy.globalAmplitude_);
	else
		globalAmplitude_ = NULL;
	if(copy.harmonicRolloff_ != NULL)
		harmonicRolloff_ = new Parameter(*copy.harmonicRolloff_);
	else
		harmonicRolloff_ = NULL;	
	if(copy.decayRate_ != NULL)
		decayRate_ = new Parameter(*copy.decayRate_);
	else
		decayRate_ = NULL;		
	
	// Initialize (don't copy) the mutexes
	if(pthread_mutex_init(&parameterMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize parameter mutex in ResonanceSynth\n";
		// Throw exception?
	}
	if(pthread_mutex_init(&noteMutex_, NULL) != 0)
	{
		cerr << "Warning: Failed to initialize note mutex in ResonanceSynth\n";
		// Throw exception?
	}	
}

void ResonanceSynth::setGlobalAmplitude(double currentAmplitude, timedParameter& rampAmplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	globalAmplitude_->setRampValues(currentAmplitude, rampAmplitude);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void ResonanceSynth::setHarmonicRolloff(double currentRolloff, timedParameter& rampRolloff)
{
	pthread_mutex_lock(&parameterMutex_);
	
	harmonicRolloff_->setRampValues(currentRolloff, rampRolloff);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void ResonanceSynth::setDecayRate(double currentRate, timedParameter& rampRate)
{
	pthread_mutex_lock(&parameterMutex_);
	
	decayRate_->setRampValues(currentRate, rampRate);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void ResonanceSynth::setMono(bool mono)
{
	pthread_mutex_lock(&parameterMutex_);
	
	mono_ = mono;
	
	pthread_mutex_unlock(&parameterMutex_);	
}


void ResonanceSynth::appendGlobalAmplitude(timedParameter& amplitude)
{
	pthread_mutex_lock(&parameterMutex_);
	
	globalAmplitude_->appendRampValues(amplitude);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void ResonanceSynth::appendHarmonicRolloff(timedParameter& rolloff)
{
	pthread_mutex_lock(&parameterMutex_);
	
	harmonicRolloff_->appendRampValues(rolloff);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

void ResonanceSynth::appendDecayRate(timedParameter& rate)
{
	pthread_mutex_lock(&parameterMutex_);
	
	decayRate_->appendRampValues(rate);
	
	pthread_mutex_unlock(&parameterMutex_);	
}

// Add a new harmonic to the list to render.  midiNoteKey is usually the midi ID of the
// note which triggered this harmonic, and frequency and amplitude tell us the initial values
// of the harmonic.

void ResonanceSynth::addHarmonic(int midiNoteKey, float frequency, float amplitude)
{
	harmonicInfo newHarmonic;
	
	if(amplitude == 0.0)		// Note off messages might generate a new harmonic otherwise
		return;
	
	newHarmonic.frequency = frequency;
	newHarmonic.phase = 0.0;
	
	pthread_mutex_lock(&parameterMutex_);
	
	float freqRatio = MIDDLE_C/frequency;
	newHarmonic.amplitude = new EnvelopeFollower(decayRate_->currentValue()*freqRatio, sampleRate_);
	
	// Start the first sample from which the amplitude will decay
	float test = newHarmonic.amplitude->filter(amplitude*powf(harmonicRolloff_->currentValue(), log2f(freqRatio)));
#ifdef DEBUG_MESSAGES
	cout << "starting amplitude " << test << endl;
#endif

	pthread_mutex_unlock(&parameterMutex_);										
												 
	pthread_mutex_lock(&noteMutex_);
	
	if(mono_)								// Allow only one note at a time
	{
		map<unsigned int, harmonicInfo>::iterator it = harmonics_.begin();
		
		while(it != harmonics_.end())
			(*it++).second.amplitude->updateTimeConstant(.01);	// This will trail the note off rapidly without a click
	}
	
	if(harmonics_.count(midiNoteKey) > 0)	// Erase any duplicate harmonics
	{
		if(amplitude > harmonics_[midiNoteKey].amplitude->currentValue())	// Only erase if the new amplitude is higher
		{
			delete harmonics_[midiNoteKey].amplitude;
			harmonics_.erase(midiNoteKey);
			harmonics_[midiNoteKey] = newHarmonic;	// Add the new harmonic
		}
	}
	else
		harmonics_[midiNoteKey] = newHarmonic;	// Add the new harmonic
	
	pthread_mutex_unlock(&noteMutex_);
}


// Render one buffer of output.  input holds the incoming audio data.  output may already contain
// audio, so we add our result to it rather than replacing.

int ResonanceSynth::render(const void *input, void *output, unsigned long frameCount,
					   const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags)
{
	float *outBuffer = (float *)output;
	float outSample;
	PaTime bufferStartTime, bufferEndTime;
	map<unsigned int, harmonicInfo>::iterator it;
	unsigned long lastFrame = frameCount;
	unsigned long i;
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
	
	// If the note has been set to release, check whether that should happen immediately, within this
	// buffer, or later.  If later, then go about our business like normal.
	if(isReleasing_)
	{
		if(releaseTime_ <= bufferStartTime)		// Release immediately
		{
			lastFrame = 0;
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			isFinished_ = true;
#ifdef DEBUG_MESSAGES
			cout << "ResonanceSynth releasing immediately" << endl;
#endif
		}
		else if(releaseTime_ < bufferEndTime)		// Release mid-buffer
		{
			lastFrame = (unsigned long)((releaseTime_ - bufferStartTime)*sampleRate_);
#ifdef DEBUG_MESSAGES
			cout << "ResonanceSynth releasing at sample " << lastFrame << endl;
#endif
			isReleasing_ = isRunning_ = false;	// These won't be checked again until next callback
			willFinishAtEnd = true;				// Set the finished flag *after* rendering
		}
		// else do nothing
	}
	
	for(i = 0; i < lastFrame; i++)
	{
		// Handle ramped parameter updates, but not every sample to save CPU time.
		// A parameter needs ramping when there is at least one item in its timedParameter deque.
		if(sampleNumber_ % PARAMETER_UPDATE_INTERVAL == 0)
		{
			pthread_mutex_lock(&parameterMutex_);	// Acquire lock so the parameters don't change
			
			globalAmplitude_->ramp(PARAMETER_UPDATE_INTERVAL);
			harmonicRolloff_->ramp(PARAMETER_UPDATE_INTERVAL);
			decayRate_->ramp(PARAMETER_UPDATE_INTERVAL);
			
			pthread_mutex_unlock(&parameterMutex_);
		}	
		
		outSample = 0.0;
		
		pthread_mutex_lock(&noteMutex_);
		
		// Go through the map of current harmonics and add the contribution from each to the output
		it = harmonics_.begin();
		
		while(it != harmonics_.end())
		{
			float frequency = (*it).second.frequency;
			EnvelopeFollower *amp = (*it).second.amplitude;
			float phase = (*it).second.phase;
			
			float newAmp = amp->filter(0.0);		// Do a rolloff on this amplitude
			phase = fmod(phase + frequency*sampleLength_, 1.0);			
			(*it).second.phase = phase;			// Increment the phase
			
			if(newAmp < 0.001) // -60dB as cutoff
			{
				delete amp;
				harmonics_.erase((*it++).first);
			}
			else
			{
				outSample += newAmp * waveTable.lookup(waveTableSine, phase);
				
				it++;
			}
		}
		
		pthread_mutex_unlock(&noteMutex_);
		
		outSample *= globalAmplitude_->currentValue();	// Scale by overall output level
		
		// Mix the output into the buffer
		outBuffer[outputChannel_] += outSample;
		
		// Update counters for next cycle
		outBuffer += numOutputChannels_;
		sampleNumber_++;
	}
	
	if(willFinishAtEnd)
		isFinished_ = true;
	
	return paContinue;
}



ResonanceSynth::~ResonanceSynth()
{
#ifdef DEBUG_ALLOCATION
	cout << "*** ~ResonanceSynth\n";
#endif
	map<unsigned int, harmonicInfo>::iterator it = harmonics_.begin();
	
	while(it != harmonics_.end())
	{
		delete (*it).second.amplitude;
		harmonics_.erase((*it++).first);
	}
	
	delete globalAmplitude_;
	delete harmonicRolloff_;
	delete decayRate_;
	
	pthread_mutex_destroy(&parameterMutex_);	
	pthread_mutex_destroy(&noteMutex_);
}
