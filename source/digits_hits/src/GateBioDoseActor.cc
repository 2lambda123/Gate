/*----------------------
Copyright (C): OpenGATE Collaboration

This software is distributed under the terms
of the GNU Lesser General  Public Licence (LGPL)
See LICENSE.md for further details
----------------------*/

#include "G4EmParameters.hh"
#include "GateBioDoseActor.hh"

#define GATE_BUFFERSIZE

//-----------------------------------------------------------------------------
GateBioDoseActor::GateBioDoseActor(G4String name, G4int depth):
	GateVImageActor(name, depth),
	_currentEvent(0),
	_messenger(this),
	_alphaRef(-1),
	_betaRef(-1),
	_enableDose(false),
	_enableBioDose(true),
	_enableAlphaMix(false),
	_enableBetaMix(false),
	_enableRBE(false)
{
	GateDebugMessageInc("Actor", 4, "GateBioDoseActor() -- begin\n");
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void GateBioDoseActor::Construct() {
	GateDebugMessageInc("Actor", 4, "GateBioDoseActor -- Construct - begin\n");
	GateVImageActor::Construct();

	// Find G4_WATER
	G4NistManager::Instance()->FindOrBuildMaterial("G4_WATER");
	// Find OtherMaterial
	//G4NistManager::Instance()->FindOrBuildMaterial(mOtherMaterial);
	G4EmParameters::Instance()->SetBuildCSDARange(true);

	// Enable callbacks BioDose
	EnableBeginOfRunAction(true);
	EnableEndOfRunAction(true);
	EnableBeginOfEventAction(true);
	EnablePreUserTrackingAction(false);
	EnablePostUserTrackingAction(false);
	EnableUserSteppingAction(true);

	// Outputs
	{
		G4String basename = removeExtension(mSaveFilename);
		G4String ext = getExtension(mSaveFilename);

		if(_enableDose) {
			G4String filename = basename + "_dose." + ext;

			SetOriginTransformAndFlagToImage(_doseImage);
			_doseImage.SetResolutionAndHalfSize(mResolution, mHalfSize, mPosition);
			_doseImage.Allocate();
			_doseImage.SetFilename(filename);
		}

		if(_enableBioDose) {
			G4String filename = basename + "_biodose." + ext;

			SetOriginTransformAndFlagToImage(_bioDoseImage);
			_bioDoseImage.SetResolutionAndHalfSize(mResolution, mHalfSize, mPosition);
			_bioDoseImage.Allocate();
			_bioDoseImage.SetFilename(filename);
		}

		if(_enableAlphaMix) {
			G4String filename = basename + "_alphamix." + ext;

			SetOriginTransformAndFlagToImage(_alphaMixImage);
			_alphaMixImage.SetResolutionAndHalfSize(mResolution, mHalfSize, mPosition);
			_alphaMixImage.Allocate();
			_alphaMixImage.SetFilename(filename);
		}

		if(_enableBetaMix) {
			G4String filename = basename + "_betamix." + ext;

			SetOriginTransformAndFlagToImage(_betaMixImage);
			_betaMixImage.SetResolutionAndHalfSize(mResolution, mHalfSize, mPosition);
			_betaMixImage.Allocate();
			_betaMixImage.SetFilename(filename);
		}

		if(_enableRBE) {
			G4String filename = basename + "_rbe." + ext;

			SetOriginTransformAndFlagToImage(_RBEImage);
			_RBEImage.SetResolutionAndHalfSize(mResolution, mHalfSize, mPosition);
			_RBEImage.Allocate();
			_RBEImage.SetFilename(filename);
		}
	}

	//ResetData();

	///////////////////////////////////////////////////////////////////////////////////////////
	//Just matrix information
	G4cout << "Memory space to store physical dose into " << mResolution.x() * mResolution.y() * mResolution.z() << " voxels has been allocated " << G4endl;

	// SOBP
	if(_SOBPWeight == 0) { _SOBPWeight = 1; }

	//Building the cell line information
	_dataBase = "data/" + _cellLine + "_" + _bioPhysicalModel + ".db";
	BuildDatabase();

	if(_alphaRef < 0 || _betaRef < 0)
		GateError("BioDoseActor " << GetName() << ": setAlphaRef and setBetaRef must be done");
}
//-----------------------------------------------------------------------------
//----------------------------------------------------------------------------- // ok bio dose
void GateBioDoseActor::BuildDatabase() {
	std::ifstream f(_dataBase);
	if(!f) GateError("BioDoseActor " << GetName() << ": unable to open file '" << _dataBase << "'");

	int nZ = 0;
	double prevKineticEnergy = 1, prevAlpha = 1, prevBeta =1;

	for(std::string line; std::getline(f, line); ) {
		std::istringstream iss(line);
		std::string firstCol;

		iss >> firstCol;

		if(firstCol == "Fragment") {
			if(nZ != 0) // prevKineticEnergy is the maximum kinetic energy for current nZ
				_energyMaxForZ[nZ] = prevKineticEnergy;

			iss >> nZ;
			prevKineticEnergy = 1;
			prevAlpha = 1;
			prevBeta = 1;
		} else if(nZ != 0) {
			double kineticEnergy, alpha, beta;
			std::istringstream{firstCol} >> kineticEnergy;
			iss >> alpha >> beta;

			auto alphaCoeff = Interpol(prevKineticEnergy, kineticEnergy, prevAlpha, alpha);
			auto betaCoeff = Interpol(prevKineticEnergy, kineticEnergy, prevBeta, beta);

			// Saving the in the input databse
			Fragment fragment{nZ, kineticEnergy};
			_alphaBetaInterpolTable[fragment] = {alphaCoeff, betaCoeff};

			prevKineticEnergy = kineticEnergy;
			prevAlpha = alpha;
			prevBeta = beta;
		} else {
			GateError("BioDoseActor " << GetName() << ": bad database format in '" << _dataBase << "'");
		}
	}

	if(nZ != 0) // last line read; prevKineticEnergy is the maximum kinetic energy for current nZ
		_energyMaxForZ[nZ] = prevKineticEnergy;
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
GateBioDoseActor::Coefficients GateBioDoseActor::Interpol(double x1, double x2, double y1, double y2) {
	//Function for a 1D linear interpolation. It returns a pair of a and b coefficients
	double a = (y2 - y1) / (x2 - x1);
	double b = y1 - x1 * a;
	return {a, b};
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
/// Save data
void GateBioDoseActor::SaveData() {
	// Calculate Alpha Beta mix
	for(auto const& [index, deposited]: _depositedMap) {
		// Alpha Beta mix (final)
		double alphamix = (deposited.alpha / deposited.energy);
		double betamix = (deposited.beta / deposited.energy);

		// Calculate dose, dosebio
		constexpr double MeVtoJoule = 1.60218e-13;
		double dose = 0;
		double biodose = 0;

		if(deposited.mass != 0)
			dose = deposited.energy * MeVtoJoule / deposited.mass;

		if(dose != 0 && alphamix != 0 && betamix != 0)
			biodose = (-_alphaRef + std::sqrt((_alphaRef * _alphaRef) + 4 * _betaRef * (alphamix * dose + betamix * (dose * dose)))) / (2 * _betaRef);

		// Write data
		if(_enableDose)     _doseImage.AddValue(index, dose);
		if(_enableBioDose)  _bioDoseImage.AddValue(index, biodose);
		if(_enableAlphaMix) _alphaMixImage.AddValue(index, alphamix);
		if(_enableBetaMix)  _betaMixImage.AddValue(index, betamix);
		if(_enableRBE)      _RBEImage.AddValue(index, dose > 0? biodose/dose : 0);
	}
	//-------------------------------------------------------------
	GateVActor::SaveData();

	if(_enableDose)     _doseImage.SaveData(_currentEvent);
	if(_enableBioDose)  _bioDoseImage.SaveData(_currentEvent);
	if(_enableAlphaMix) _alphaMixImage.SaveData(_currentEvent);
	if(_enableBetaMix)  _betaMixImage.SaveData(_currentEvent);
	if(_enableRBE)      _RBEImage.SaveData(_currentEvent);
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void GateBioDoseActor::BeginOfRunAction(const G4Run* r) {
	GateVActor::BeginOfRunAction(r);
	GateDebugMessage("Actor", 3, "GateBioDoseActor -- Begin of Run\n");
}
//-----------------------------------------------------------------------------
//----------------------------------------------------------------------------- // PAS DANS DOSE ACTOR
void GateBioDoseActor::EndOfRunAction(const G4Run* r) {
	GateVActor::EndOfRunAction(r);
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Callback at each event
void GateBioDoseActor::BeginOfEventAction(const G4Event* e) {
	GateVActor::BeginOfEventAction(e);
	++_currentEvent;
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void GateBioDoseActor::UserSteppingActionInVoxel(const int index, const G4Step* step) {
	double const energyDep = step->GetTotalEnergyDeposit();

	// Get information from step
	// Particle
	G4int nZ = step->GetTrack()->GetDefinition()->GetAtomicNumber();
	double kineticEnergyPerNucleon = (step->GetPreStepPoint()->GetKineticEnergy()) / (step->GetTrack()->GetDefinition()->GetAtomicMass()); //OK

	DepositedMap::iterator it = _depositedMap.find(index);

	if(energyDep > 0) {
		if(it == std::end(_depositedMap)) {
			_depositedMap[index] = {0, 0, 0};
			it = _depositedMap.find(index);
		}

		auto& deposited = (*it).second;

		// Accumulate energy inconditionnaly
		deposited.energy += energyDep;

		auto current_material = step->GetPreStepPoint()->GetMaterial();
		double density = current_material->GetDensity();
		deposited.mass = ((1 * (mVoxelSize.x() / cm * mVoxelSize.y() / cm * mVoxelSize.z() / cm)) * density);

		// Accumulation of alpha/beta if ion type if known
		// -> check if the ion type is known
		if(_energyMaxForZ.count(nZ)) {
			//The max values in the database aren't being taking into account
			//so for now it's coded like this to be sure the code takes them into account
			double energyMax = _energyMaxForZ.at(nZ);

			AlphaBetaInterpolTable::const_iterator itr2;
			if (kineticEnergyPerNucleon >= energyMax) {
				// If the kinetic energy is the maximum value in the alpha beta tables,
				// we have to use the a and b coefficient for this maximum value
				Fragment fragmentKineticEnergyMax{nZ, energyMax};
				itr2 = _alphaBetaInterpolTable.find(fragmentKineticEnergyMax);
			} else {
				// We pair the ion type and the kinetic energy
				Fragment fragmentKineticEnergy{nZ, kineticEnergyPerNucleon};
				itr2 = _alphaBetaInterpolTable.upper_bound(fragmentKineticEnergy);
			}

			// Calculation of EZ, alphaDep and betaDep (K = a*EZ+b*E)
			auto const& interpol = (*itr2).second;

			double ez = kineticEnergyPerNucleon * energyDep;
			double alphaDep = interpol.alpha.a  * ez + interpol.alpha.b * energyDep;
			double betaDep  = interpol.beta.a   * ez + interpol.beta.b  * energyDep;

			// Accumulate alpha/beta
			deposited.alpha   += alphaDep;
			deposited.beta    += betaDep;
		}
	}
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void GateBioDoseActor::ResetData() {
}
//-----------------------------------------------------------------------------
