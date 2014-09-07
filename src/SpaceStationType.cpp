// Copyright © 2008-2014 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "SpaceStationType.h"
#include "FileSystem.h"
#include "Lua.h"
#include "LuaVector.h"
#include "LuaVector.h"
#include "LuaTable.h"
#include "Pi.h"
#include "MathUtil.h"
#include "Ship.h"
#include "StringF.h"
#include "scenegraph/Model.h"
#include "OS.h"

#include <algorithm>

static lua_State *s_lua;
static std::string s_currentStationFile = "";
std::vector<SpaceStationType> SpaceStationType::surfaceStationTypes;
std::vector<SpaceStationType> SpaceStationType::orbitalStationTypes;

SpaceStationType::SpaceStationType()
: id("")
, model(0)
, modelName("")
, angVel(0.f)
, dockMethod(SURFACE)
, numDockingPorts(0)
, numDockingStages(0)
, numUndockStages(0)
, shipLaunchStage(3)
, parkingDistance(0)
, parkingGapSize(0)
{}

void SpaceStationType::OnSetupComplete()
{
	// Since the model contains (almost) all of the docking information we have to extract that
	// and then generate any additional locators and information the station will need from it.

	// First we gather the MatrixTransforms that contain the location and orientation of the docking
	// locators/waypoints. We store some information within the name of these which needs parsing too.

	// Next we build the additional information required for docking ships with SPACE stations
	// on autopilot - this is the only option for docking with SPACE stations currently.
	// This mostly means offsetting from one locator to create the next in the sequence.

	// ground stations have a "special-fucking-case" 0 stage launch process
	shipLaunchStage = ((SURFACE==dockMethod) ? 0 : 3);

	// gather the tags
	SceneGraph::Model::TVecMT entrance_mts;
	SceneGraph::Model::TVecMT locator_mts;
	SceneGraph::Model::TVecMT exit_mts;
	model->FindTagsByStartOfName("entrance_", entrance_mts);
	model->FindTagsByStartOfName("loc_", locator_mts);
	model->FindTagsByStartOfName("exit_", exit_mts);

	// Add the partially initialised ports
	for (auto apprIter : entrance_mts)
	{
		int portId;
		PiVerify(1 == sscanf(apprIter->GetName().c_str(), "entrance_port%d", &portId));
		PiVerify(portId>0);

		SPort new_port;
		new_port.portId = portId;
		new_port.name = apprIter->GetName();
		if(SURFACE==dockMethod) {
			const vector3f offDir = apprIter->GetTransform().Up().Normalized();
			new_port.m_approach[1] = apprIter->GetTransform();
			new_port.m_approach[1].SetTranslate( apprIter->GetTransform().GetTranslate() + (offDir * 500.0f) );
		} else {
			const vector3f offDir = -apprIter->GetTransform().Back().Normalized();
			new_port.m_approach[1] = apprIter->GetTransform();
			new_port.m_approach[1].SetTranslate( apprIter->GetTransform().GetTranslate() + (offDir * 1500.0f) );
		}
		new_port.m_approach[2] = apprIter->GetTransform();
		m_ports.push_back( new_port );
	}

	for (auto locIter : locator_mts)
	{
		int bay, portId;
		int minSize, maxSize;
		char padname[8];
		const matrix4x4f &locTransform = locIter->GetTransform();

		// eg:loc_A001_p01_s0_500_b01
		PiVerify(5 == sscanf(locIter->GetName().c_str(), "loc_%4s_p%d_s%d_%d_b%d", &padname, &portId, &minSize, &maxSize, &bay));
		PiVerify(bay>0 && portId>0);

		// find the port and setup the rest of it's information
		bool bFoundPort = false;
		matrix4x4f approach1;
		matrix4x4f approach2;
		for(auto &rPort : m_ports) {
			if(rPort.portId == portId) {
				rPort.minShipSize = std::min(minSize,rPort.minShipSize);
				rPort.maxShipSize = std::max(maxSize,rPort.maxShipSize);
				rPort.bayIDs.push_back( std::make_pair(bay-1, padname) );
				bFoundPort = true;
				approach1 = rPort.m_approach[1];
				approach2 = rPort.m_approach[2];
				break;
			}
		}
		assert(bFoundPort);

		// now build the docking/leaving waypoints
		if( SURFACE == dockMethod )
		{
			// ground stations don't have leaving waypoints.
			m_portPaths[bay].m_docking[2] = locTransform; // final (docked)
			numDockingStages = 2;
			numUndockStages = 1;
		}
		else
		{
			struct SPlane
			{
				static vector3f IntersectLine(const vector3f &n, const vector3f &p0, const vector3f &a, const vector3f &b) {
					using namespace MathUtil;
					const vector3f ba = b-a;
					const float d = p0.Length();
					const float nDotA = Dot(n, a);
					const float nDotBA = Dot(n, ba);

					return a + (((d - nDotA)/nDotBA) * ba);
				}
			};

			// create the docking locators
			// start
			m_portPaths[bay].m_docking[2] = approach2;
			m_portPaths[bay].m_docking[2].SetRotationOnly( locTransform.GetOrient() );
			// above the pad
			vector3f intersectionPos;
			const vector3f approach1Pos = approach1.GetTranslate();
			const vector3f approach2Pos = approach2.GetTranslate();
			{
				const vector3f nUp = locTransform.Up().Normalized(); // plane normal
				const vector3f nBack = locTransform.Back().Normalized(); // plane normal
				const vector3f nRight = locTransform.Right().Normalized(); // plane normal
				const vector3f p0 = locTransform.GetTranslate(); // plane position
				const vector3f l = (approach2Pos - approach1Pos).Normalized(); // ray direction
				const vector3f l0 = approach1Pos + (l*10000.0f);
				
				// back intersection default
				intersectionPos = SPlane::IntersectLine(nBack, p0, approach1Pos, l0);
				// right check
				vector3f ip = SPlane::IntersectLine(nRight, p0, approach1Pos, l0);
				if((intersectionPos - p0).LengthSqr() > (ip - p0).LengthSqr())
					intersectionPos = ip;
				// up check
				ip = SPlane::IntersectLine(nUp, p0, approach1Pos, l0);
				if((intersectionPos - p0).LengthSqr() > (ip - p0).LengthSqr())
					intersectionPos = ip;
			}
			m_portPaths[bay].m_docking[3] = locTransform;
			m_portPaths[bay].m_docking[3].SetTranslate( intersectionPos );
			// final (docked)
			m_portPaths[bay].m_docking[4] = locTransform;
			numDockingStages = 4;

			// leaving locators ...
			matrix4x4f orient = locTransform.GetOrient(), EndOrient;
			if( exit_mts.empty() )
			{
				// leaving locators need to face in the opposite direction
				const matrix4x4f rot = matrix3x3f::Rotate(DEG2RAD(180.0f), orient.Back());
				orient = orient * rot;
				orient.SetTranslate( locTransform.GetTranslate() );
				EndOrient = approach2;
				EndOrient.SetRotationOnly(orient);
			}
			else
			{
				// leaving locators, use whatever orientation they have
				orient.SetTranslate( locTransform.GetTranslate() );
				int exitport = 0;
				for( auto &exitIt : exit_mts ) {
					PiVerify(1 == sscanf( exitIt->GetName().c_str(), "exit_port%d", &exitport ));
					if( exitport == portId )
					{
						EndOrient = exitIt->GetTransform();
						break;
					}
				}
				if( exitport == 0 )
				{
					EndOrient = approach2;
				}
			}

			// create the leaving locators
			m_portPaths[bay].m_leaving[1] = locTransform; // start - maintain the same orientation and position as when docked.
			m_portPaths[bay].m_leaving[2] = orient; // above the pad - reorient...
			m_portPaths[bay].m_leaving[2].SetTranslate( intersectionPos ); //  ...and translate to new position
			m_portPaths[bay].m_leaving[3] = EndOrient; // end (on manual after here)
			numUndockStages = 3;
		}
	}
	
	numDockingPorts = m_portPaths.size();

	// sanity
	assert(!m_portPaths.empty());
	assert(numDockingStages > 0);
	assert(numUndockStages > 0);

	// insanity
	for (PortPathMap::const_iterator pIt = m_portPaths.begin(), pItEnd = m_portPaths.end(); pIt!=pItEnd; ++pIt)
	{
		if (Uint32(numDockingStages-1) < pIt->second.m_docking.size()) {
			Error(
				"(%s): numDockingStages (%d) vs number of docking stages (" SIZET_FMT ")\n"
				"Must have at least the same number of entries as the number of docking stages "
				"PLUS the docking timeout at the start of the array.",
				modelName.c_str(), (numDockingStages-1), pIt->second.m_docking.size());

		} else if (Uint32(numDockingStages-1) != pIt->second.m_docking.size()) {
			Warning(
				"(%s): numDockingStages (%d) vs number of docking stages (" SIZET_FMT ")\n",
				modelName.c_str(), (numDockingStages-1), pIt->second.m_docking.size());
		}

		if (0!=pIt->second.m_leaving.size() && Uint32(numUndockStages) < pIt->second.m_leaving.size()) {
			Error(
				"(%s): numUndockStages (%d) vs number of leaving stages (" SIZET_FMT ")\n"
				"Must have at least the same number of entries as the number of leaving stages.",
				modelName.c_str(), (numDockingStages-1), pIt->second.m_docking.size());

		} else if(0!=pIt->second.m_leaving.size() && Uint32(numUndockStages) != pIt->second.m_leaving.size()) {
			Warning(
				"(%s): numUndockStages (%d) vs number of leaving stages (" SIZET_FMT ")\n",
				modelName.c_str(), numUndockStages, pIt->second.m_leaving.size());
		}

	}
}

const SpaceStationType::SPort* SpaceStationType::FindPortByBay(const int zeroBaseBayID) const
{
	for (TPorts::const_iterator bayIter = m_ports.begin(), grpEnd=m_ports.end(); bayIter!=grpEnd ; ++bayIter ) {
		for (auto idIter : (*bayIter).bayIDs ) {
			if (idIter.first==zeroBaseBayID) {
				return &(*bayIter);
			}
		}
	}
	// is it safer to return that the bay is locked?
	return 0;
}

SpaceStationType::SPort* SpaceStationType::GetPortByBay(const int zeroBaseBayID)
{
	for (TPorts::iterator bayIter = m_ports.begin(), grpEnd=m_ports.end(); bayIter!=grpEnd ; ++bayIter ) {
		for (auto idIter : (*bayIter).bayIDs ) {
			if (idIter.first==zeroBaseBayID) {
				return &(*bayIter);
			}
		}
	}
	// is it safer to return that the bay is locked?
	return 0;
}

bool SpaceStationType::GetShipApproachWaypoints(const unsigned int port, const int stage, positionOrient_t &outPosOrient) const
{
	bool gotOrient = false;

	const SPort* pPort = FindPortByBay(port);
	if (pPort && stage>0) {
		TMapBayIDMat::const_iterator stageDataIt = pPort->m_approach.find(stage);
		if (stageDataIt != pPort->m_approach.end()) {
			const matrix4x4f &mt = pPort->m_approach.at(stage);
			outPosOrient.pos	= vector3d(mt.GetTranslate());
			outPosOrient.xaxis	= vector3d(mt.GetOrient().VectorX());
			outPosOrient.yaxis	= vector3d(mt.GetOrient().VectorY());
			outPosOrient.zaxis	= vector3d(mt.GetOrient().VectorZ());
			outPosOrient.xaxis	= outPosOrient.xaxis.Normalized();
			outPosOrient.yaxis	= outPosOrient.yaxis.Normalized();
			outPosOrient.zaxis	= outPosOrient.zaxis.Normalized();
			gotOrient = true;
		}
	}
	return gotOrient;
}

double SpaceStationType::GetDockAnimStageDuration(const int stage) const
{
	return (stage==0) ? 300.0 : ((SURFACE==dockMethod) ? 0.0 : 3.0);
}

double SpaceStationType::GetUndockAnimStageDuration(const int stage) const
{
	return ((SURFACE==dockMethod) ? 0.0 : 5.0);
}

static bool GetPosOrient(const SpaceStationType::TMapBayIDMat &bayMap, const int stage, const double t, const vector3d &from,
				  SpaceStationType::positionOrient_t &outPosOrient)
{
	bool gotOrient = false;

	vector3d toPos;

	const SpaceStationType::TMapBayIDMat::const_iterator stageDataIt = bayMap.find( stage );
	const bool bHasStageData = (stageDataIt != bayMap.end());
	assert(bHasStageData);
	if (bHasStageData) {
		const matrix4x4f &mt = stageDataIt->second;
		outPosOrient.xaxis	= vector3d(mt.GetOrient().VectorX()).Normalized();
		outPosOrient.yaxis	= vector3d(mt.GetOrient().VectorY()).Normalized();
		outPosOrient.zaxis	= vector3d(mt.GetOrient().VectorZ()).Normalized();
		toPos				= vector3d(mt.GetTranslate());
		gotOrient = true;
	}

	if (gotOrient)
	{
		vector3d pos		= MathUtil::mix<vector3d, double>(from, toPos, t);
		outPosOrient.pos	= pos;
	}

	return gotOrient;
}

/* when ship is on rails it returns true and fills outPosOrient.
 * when ship has been released (or docked) it returns false.
 * Note station animations may continue for any number of stages after
 * ship has been released and is under player control again */
bool SpaceStationType::GetDockAnimPositionOrient(const unsigned int port, int stage, double t, const vector3d &from, positionOrient_t &outPosOrient, const Ship *ship) const
{
	assert(ship);
	if (stage < -shipLaunchStage) { stage = -shipLaunchStage; t = 1.0; }
	if (stage > numDockingStages || !stage) { stage = numDockingStages; t = 1.0; }
	// note case for stageless launch (shipLaunchStage==0)

	bool gotOrient = false;

	assert(port<=m_portPaths.size());
	const PortPath &rPortPath = m_portPaths.at(port+1);
	if (stage<0) {
		const int leavingStage = (-1*stage);
		gotOrient = GetPosOrient(rPortPath.m_leaving, leavingStage, t, from, outPosOrient);
		const vector3d up = outPosOrient.yaxis.Normalized() * ship->GetLandingPosOffset();
		outPosOrient.pos = outPosOrient.pos - up;
	} else if (stage>0) {
		gotOrient = GetPosOrient(rPortPath.m_docking, stage, t, from, outPosOrient);
		const vector3d up = outPosOrient.yaxis.Normalized() * ship->GetLandingPosOffset();
		outPosOrient.pos = outPosOrient.pos - up;
	}

	return gotOrient;
}

static int _define_station(lua_State *L, SpaceStationType &station)
{
	station.id = s_currentStationFile;

	LUA_DEBUG_START(L);
	LuaTable t(L, -1);
	station.modelName = t.Get<std::string>("model");
	station.angVel = t.Get("angular_velocity", 0.f);
	station.parkingDistance = t.Get("parking_distance", 5000.f);
	station.parkingGapSize = t.Get("parking_gap_size", 2000.f);
	station.padOffset = t.Get("pad_offset", 150.f);
	LUA_DEBUG_END(L, 0);

	assert(!station.modelName.empty());

	station.model = Pi::FindModel(station.modelName);
	station.OnSetupComplete();
	return 0;
}

static int define_orbital_station(lua_State *L)
{
	SpaceStationType station;
	station.dockMethod = SpaceStationType::ORBITAL;
	_define_station(L, station);
	SpaceStationType::orbitalStationTypes.push_back(station);
	return 0;
}

static int define_surface_station(lua_State *L)
{
	SpaceStationType station;
	station.dockMethod = SpaceStationType::SURFACE;
	_define_station(L, station);
	SpaceStationType::surfaceStationTypes.push_back(station);
	return 0;
}

void SpaceStationType::Init()
{
	assert(s_lua == 0);
	if (s_lua != 0) return;

	s_lua = luaL_newstate();
	lua_State *L = s_lua;

	LUA_DEBUG_START(L);
	pi_lua_open_standard_base(L);

	LuaVector::Register(L);

	LUA_DEBUG_CHECK(L, 0);

	lua_register(L, "define_orbital_station", define_orbital_station);
	lua_register(L, "define_surface_station", define_surface_station);

	namespace fs = FileSystem;
	for (fs::FileEnumerator files(fs::gameDataFiles, "stations", fs::FileEnumerator::Recurse);
			!files.Finished(); files.Next()) {
		const fs::FileInfo &info = files.Current();
		if (ends_with_ci(info.GetPath(), ".lua")) {
			const std::string name = info.GetName();
			s_currentStationFile = name.substr(0, name.size()-4);
			pi_lua_dofile(L, info.GetPath());
			s_currentStationFile.clear();
		}
	}
	LUA_DEBUG_END(L, 0);
}

void SpaceStationType::Uninit()
{
	lua_close(s_lua); s_lua = 0;
}
