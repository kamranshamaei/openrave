// -*- coding: utf-8 -*-
// Copyright (C) 2006-2010 Rosen Diankov (rosen.diankov@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#ifndef  BIRRT_PLANNER_H
#define  BIRRT_PLANNER_H

#include "rplanners.h"
#include <boost/algorithm/string.hpp>

template <typename Node>
class RrtPlanner : public PlannerBase
{
public:

    RrtPlanner(EnvironmentBasePtr penv) : PlannerBase(penv), _treeForward(0)
    {
        __description = "\
:Interface Author:  Rosen Diankov\n\n\
Uses the Rapidly-Exploring Random Trees Algorithm.\n\
";
        RegisterCommand("GetGoalIndex",boost::bind(&RrtPlanner<Node>::GetGoalIndexCommand,this,_1,_2),
                        "returns the goal index of the plan");
        RegisterCommand("GetInitGoalIndices",boost::bind(&RrtPlanner<Node>::GetInitGoalIndicesCommand,this,_1,_2),
                        "returns the start and goal indices");
    }
    virtual ~RrtPlanner() {
    }

    virtual bool _InitPlan(RobotBasePtr pbase, PlannerParametersPtr params)
    {
        params->Validate();
        _goalindex = -1;
        _startindex = -1;
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        if( !_uniformsampler ) {
            _uniformsampler = RaveCreateSpaceSampler(GetEnv(),"mt19937");
        }
        _robot = pbase;

        _uniformsampler->SetSeed(params->_nRandomGeneratorSeed);
        FOREACH(it, params->_listInternalSamplers) {
            (*it)->SetSeed(params->_nRandomGeneratorSeed);
        }

        PlannerParameters::StateSaver savestate(params);
        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);

        if( (int)params->vinitialconfig.size() % params->GetDOF() ) {
            RAVELOG_ERROR(str(boost::format("initial config wrong dim: %d %% %d != 0\n")%params->vinitialconfig.size()%params->GetDOF()));
            return false;
        }

        _vecInitialNodes.resize(0);
        _sampleConfig.resize(params->GetDOF());
        _treeForward.Init(shared_planner(), params->GetDOF(), params->_distmetricfn, params->_fStepLength, params->_distmetricfn(params->_vConfigLowerLimit, params->_vConfigUpperLimit));
        std::vector<dReal> vinitialconfig(params->GetDOF());
        for(size_t index = 0; index < params->vinitialconfig.size(); index += params->GetDOF()) {
            std::copy(params->vinitialconfig.begin()+index,params->vinitialconfig.begin()+index+params->GetDOF(),vinitialconfig.begin());
            if( params->CheckPathAllConstraints(vinitialconfig,vinitialconfig, std::vector<dReal>(), std::vector<dReal>(), 0, IT_OpenStart) != 0 ) {
                continue;
            }
            _vecInitialNodes.push_back(_treeForward.InsertNode(NULL, vinitialconfig, _vecInitialNodes.size()));
        }

        if( _treeForward.GetNumNodes() == 0 && !params->_sampleinitialfn ) {
            RAVELOG_WARN("no initial configurations\n");
            return false;
        }

        return true;
    }

    // simple path optimization
    virtual void _SimpleOptimizePath(list<Node*>& path, int numiterations)
    {
        if( path.size() <= 2 ) {
            return;
        }
        PlannerParametersConstPtr params = GetParameters();

        typename list<Node*>::iterator startNode, endNode;
        if( !_filterreturn ) {
            _filterreturn.reset(new ConstraintFilterReturn());
        }
        int dof = GetParameters()->GetDOF();
        int nrejected = 0;
        int i = numiterations;
        while(i > 0 && nrejected < (int)path.size()+4 ) {
            --i;

            // pick a random node on the path, and a random jump ahead
            int endIndex = 2+(_uniformsampler->SampleSequenceOneUInt32()%((int)path.size()-2));
            int startIndex = _uniformsampler->SampleSequenceOneUInt32()%(endIndex-1);

            startNode = path.begin();
            advance(startNode, startIndex);
            endNode = startNode;
            advance(endNode, endIndex-startIndex);
            nrejected++;

            // check if the nodes can be connected by a straight line
            _filterreturn->Clear();
            //if ( params->CheckPathAllConstraints(VectorWrapper<dReal>((*startNode)->q, dof), VectorWrapper<dReal>((*endNode)->q, dof), std::vector<dReal>(), std::vector<dReal>(), 0, IT_Open, 0xffff|CFO_FillCheckedConfiguration, _filterreturn) != 0 ) {
            if ( params->CheckPathAllConstraints(std::vector<dReal>((*startNode)->q, (*startNode)->q+dof), std::vector<dReal>((*endNode)->q, (*endNode)->q+dof), std::vector<dReal>(), std::vector<dReal>(), 0, IT_Open, 0xffff|CFO_FillCheckedConfiguration, _filterreturn) != 0 ) {
                if( nrejected++ > (int)path.size()+8 ) {
                    break;
                }
                continue;
            }

            ++startNode;
            OPENRAVE_ASSERT_OP(_filterreturn->_configurations.size()%dof,==,0);
            for(std::vector<dReal>::iterator itvalues = _filterreturn->_configurations.begin(); itvalues != _filterreturn->_configurations.end(); itvalues += dof) {
                NodeBase* pnode = _treeForward.InsertNode(NULL, std::vector<dReal>(itvalues,itvalues+dof), 0);
                path.insert(startNode, (Node*)pnode);
            }
            // splice out in-between nodes in path
            path.erase(startNode, endNode);
            nrejected = 0;

            if( path.size() <= 2 ) {
                return;
            }

            break;
        }
    }

    bool GetGoalIndexCommand(std::ostream& os, std::istream& is)
    {
        os << _goalindex;
        return !!os;
    }

    bool GetInitGoalIndicesCommand(std::ostream& os, std::istream& is)
    {
        os << _startindex << " " << _goalindex;
        return !!os;
    }

protected:
    RobotBasePtr _robot;
    std::vector<dReal> _sampleConfig;
    int _goalindex, _startindex;
    SpaceSamplerBasePtr _uniformsampler;
    ConstraintFilterReturnPtr _filterreturn;

    SpatialTree< Node > _treeForward;
    std::vector< NodeBase* > _vecInitialNodes;

    inline boost::shared_ptr<RrtPlanner> shared_planner() {
        return boost::dynamic_pointer_cast<RrtPlanner>(shared_from_this());
    }
    inline boost::shared_ptr<RrtPlanner const> shared_planner_const() const {
        return boost::dynamic_pointer_cast<RrtPlanner const>(shared_from_this());
    }
};

class BirrtPlanner : public RrtPlanner<SimpleNode>
{
public:
    BirrtPlanner(EnvironmentBasePtr penv) : RrtPlanner<SimpleNode>(penv), _treeBackward(1)
    {
        __description += "Bi-directional RRTs. See\n\n\
- J.J. Kuffner and S.M. LaValle. RRT-Connect: An efficient approach to single-query path planning. In Proc. IEEE Int'l Conf. on Robotics and Automation (ICRA'2000), pages 995-1001, San Francisco, CA, April 2000.";
        RegisterCommand("DumpTree", boost::bind(&BirrtPlanner::_DumpTreeCommand,this,_1,_2),
                        "dumps the source and goal trees to $OPENRAVE_HOME/birrtdump.txt. The first N values are the DOF values, the last value is the parent index.\n\
Some python code to display data::\n\
\n\
  sourcetree=loadtxt(os.path.join(RaveGetHomeDirectory(),'sourcetree.txt'),delimiter=' ,')\n\
  hs=env.plot3(sourcetree,5,[1,0,0])\n\
  sourcedist = abs(sourcetree[:,0]-x[0]) + abs(sourcetree[:,1]-x[1])\n\
  robot.SetActiveDOFValues(sourcetree[argmin(sourcedist)])\n\
\n\
");
        _nValidGoals = 0;
    }
    virtual ~BirrtPlanner() {
    }

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr pparams)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new RRTParameters());
        _parameters->copy(pparams);
        if( !RrtPlanner<SimpleNode>::_InitPlan(pbase,_parameters) ) {
            _parameters.reset();
            return false;
        }

        _fGoalBiasProb = dReal(0.01);
        PlannerParameters::StateSaver savestate(_parameters);
        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);

        _treeBackward.Init(shared_planner(), _parameters->GetDOF(), _parameters->_distmetricfn, _parameters->_fStepLength, _parameters->_distmetricfn(_parameters->_vConfigLowerLimit, _parameters->_vConfigUpperLimit));

        //read in all goals
        if( (_parameters->vgoalconfig.size() % _parameters->GetDOF()) != 0 ) {
            RAVELOG_ERROR("BirrtPlanner::InitPlan - Error: goals are improperly specified:\n");
            _parameters.reset();
            return false;
        }

        vector<dReal> vgoal(_parameters->GetDOF());
        _vecGoalNodes.resize(0);
        _nValidGoals = 0;
        for(size_t igoal = 0; igoal < _parameters->vgoalconfig.size(); igoal += _parameters->GetDOF()) {
            std::copy(_parameters->vgoalconfig.begin()+igoal,_parameters->vgoalconfig.begin()+igoal+_parameters->GetDOF(),vgoal.begin());
            if( _parameters->CheckPathAllConstraints(vgoal,vgoal,std::vector<dReal>(), std::vector<dReal>(), 0, IT_OpenStart) == 0 ) {
                _vecGoalNodes.push_back(_treeBackward.InsertNode(NULL, vgoal, _vecGoalNodes.size()));
                _nValidGoals++;
            }
            else {
                RAVELOG_WARN(str(boost::format("goal %d fails constraints\n")%igoal));
                _vecGoalNodes.push_back(NULL); // have to push back dummy or else indices will be messed up
            }
        }

        if( _treeBackward.GetNumNodes() == 0 && !_parameters->_samplegoalfn ) {
            RAVELOG_WARN("no goals specified\n");
            _parameters.reset();
            return false;
        }

        if( _parameters->_nMaxIterations <= 0 ) {
            _parameters->_nMaxIterations = 10000;
        }

        RAVELOG_DEBUG_FORMAT("BiRRT Planner Initialized, initial=%d, goal=%d", _vecInitialNodes.size()%_treeBackward.GetNumNodes());
        return true;
    }

    struct GOALPATH
    {
        GOALPATH() : startindex(-1), goalindex(-1), length(0) {
        }
        vector<dReal> qall;
        int startindex, goalindex;
        dReal length;
    };

    virtual PlannerStatus PlanPath(TrajectoryBasePtr ptraj)
    {
        _goalindex = -1;
        _startindex = -1;
        if(!_parameters) {
            RAVELOG_ERROR("BirrtPlanner::PlanPath - Error, planner not initialized\n");
            return PS_Failed;
        }

        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        uint32_t basetime = utils::GetMilliTime();

        // the main planning loop
        PlannerParameters::StateSaver savestate(_parameters);
        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);

        SpatialTreeBase* TreeA = &_treeForward;
        SpatialTreeBase* TreeB = &_treeBackward;
        NodeBase* iConnectedA=NULL, *iConnectedB=NULL;
        int iter = 0;

        list<GOALPATH> listgoalpaths;
        bool bSampleGoal = true;
        PlannerProgress progress;
        PlannerAction callbackaction=PA_None;
        while(listgoalpaths.size() < _parameters->_minimumgoalpaths && iter < 3*_parameters->_nMaxIterations) {
            RAVELOG_VERBOSE_FORMAT("iter=%d, forward=%d, backward=%d", (iter/3)%_treeForward.GetNumNodes()%_treeBackward.GetNumNodes());
            ++iter;

            if( !!_parameters->_samplegoalfn ) {
                vector<dReal> vgoal;
                if( _parameters->_samplegoalfn(vgoal) ) {
                    RAVELOG_VERBOSE(str(boost::format("inserting new goal index %d")%_vecGoalNodes.size()));
                    _vecGoalNodes.push_back(_treeBackward.InsertNode(NULL, vgoal, _vecGoalNodes.size()));
                    _nValidGoals++;
                }
            }
            if( !!_parameters->_sampleinitialfn ) {
                vector<dReal> vinitial;
                if( _parameters->_sampleinitialfn(vinitial) ) {
                    RAVELOG_VERBOSE(str(boost::format("inserting new initial %d")%_vecInitialNodes.size()));
                    _vecInitialNodes.push_back(_treeForward.InsertNode(NULL,vinitial, _vecInitialNodes.size()));
                }
            }

            _sampleConfig.resize(0);
            if( (bSampleGoal || _uniformsampler->SampleSequenceOneReal() < _fGoalBiasProb) && _nValidGoals > 0 ) {
                bSampleGoal = false;
                // sample goal as early as possible
                uint32_t bestgoalindex = -1;
                for(size_t testiter = 0; testiter < _vecGoalNodes.size()*3; ++testiter) {
                    uint32_t sampleindex = _uniformsampler->SampleSequenceOneUInt32();
                    uint32_t goalindex = sampleindex%_vecGoalNodes.size();
                    if( !_vecGoalNodes.at(goalindex) ) {
                        continue; // dummy
                    }
                    // make sure goal is not already found
                    bool bfound = false;
                    FOREACHC(itgoalpath,listgoalpaths) {
                        if( goalindex == (uint32_t)itgoalpath->goalindex ) {
                            bfound = true;
                            break;
                        }
                    }
                    if( !bfound) {
                        bestgoalindex = goalindex;
                        break;
                    }
                }
                if( bestgoalindex != uint32_t(-1) ) {
                    _treeBackward.GetVectorConfig(_vecGoalNodes.at(bestgoalindex), _sampleConfig);
                }
            }

            if( _sampleConfig.size() == 0 ) {
                if( !_parameters->_samplefn(_sampleConfig) ) {
                    continue;
                }
            }

            // extend A
            ExtendType et = TreeA->Extend(_sampleConfig, iConnectedA);

            // although check isn't necessary, having it improves running times
            if( et == ET_Failed ) {
                // necessary to increment iterator in case spaces are not connected
                if( iter > 3*_parameters->_nMaxIterations ) {
                    RAVELOG_WARN("iterations exceeded\n");
                    break;
                }
                continue;
            }

            et = TreeB->Extend(TreeA->GetVectorConfig(iConnectedA), iConnectedB);     // extend B toward A

            if( et == ET_Connected ) {
                // connected, process goal
                listgoalpaths.push_back(GOALPATH());
                _ExtractPath(listgoalpaths.back(), TreeA == &_treeForward ? iConnectedA : iConnectedB, TreeA == &_treeBackward ? iConnectedA : iConnectedB);
                int goalindex = listgoalpaths.back().goalindex;
                int startindex = listgoalpaths.back().startindex;
                if( IS_DEBUGLEVEL(Level_Debug) ) {
                    stringstream ss; ss << std::setprecision(std::numeric_limits<dReal>::digits10+1);
                    ss << "found a goal, start index=" << startindex << " goal index=" << goalindex << ", path length=" << listgoalpaths.back().length << ", values=[";
                    for(int i = 0; i < _parameters->GetDOF(); ++i) {
                        ss << listgoalpaths.back().qall.at(listgoalpaths.back().qall.size()-_parameters->GetDOF()+i) << ", ";
                    }
                    ss << "]";
                    RAVELOG_DEBUG(ss.str());
                }
                if( listgoalpaths.size() >= _parameters->_minimumgoalpaths || listgoalpaths.size() >= _nValidGoals ) {
                    break;
                }
                bSampleGoal = true;
                // more goals requested, so make sure to remove all the nodes pointing to the current found goal
                _treeBackward.InvalidateNodesWithParent(_vecGoalNodes.at(goalindex));
            }

            swap(TreeA, TreeB);
            iter += 3;
            if( iter > 3*_parameters->_nMaxIterations ) {
                RAVELOG_WARN("iterations exceeded\n");
                break;
            }

            progress._iteration = iter/3;
            callbackaction = _CallCallbacks(progress);
            if( callbackaction ==  PA_Interrupt ) {
                return PS_Interrupted;
            }
            else if( callbackaction == PA_ReturnWithAnySolution ) {
                if( listgoalpaths.size() > 0 ) {
                    break;
                }
            }
        }

        if( listgoalpaths.size() == 0 ) {
            RAVELOG_WARN("plan failed, %fs\n",0.001f*(float)(utils::GetMilliTime()-basetime));
            return PS_Failed;
        }

        list<GOALPATH>::iterator itbest = listgoalpaths.begin();
        FOREACH(itpath,listgoalpaths) {
            if( itpath->length < itbest->length ) {
                itbest = itpath;
            }
        }
        _goalindex = itbest->goalindex;
        _startindex = itbest->startindex;
        if( ptraj->GetConfigurationSpecification().GetDOF() == 0 ) {
            ptraj->Init(_parameters->_configurationspecification);
        }
        ptraj->Insert(ptraj->GetNumWaypoints(),itbest->qall,_parameters->_configurationspecification);
        {
            ofstream f("test.traj.xml");
            ptraj->serialize(f);
        }
        RAVELOG_DEBUG_FORMAT("plan success, iters=%d, path=%d points, computation time=%fs\n", progress._iteration%ptraj->GetNumWaypoints()%(0.001f*(float)(utils::GetMilliTime()-basetime)));
        return _ProcessPostPlanners(_robot,ptraj);
    }

    virtual void _ExtractPath(GOALPATH& goalpath, NodeBase* iConnectedForward, NodeBase* iConnectedBackward)
    {
        list<SimpleNode*> vecnodes;

        // add nodes from the forward tree
        SimpleNode* pforward = (SimpleNode*)iConnectedForward;
        goalpath.startindex = -1;
        while(1) {
            vecnodes.push_front(pforward);
            if(!pforward->rrtparent) {
                goalpath.startindex = pforward->_userdata;
                break;
            }
            pforward = pforward->rrtparent;
        }

        // add nodes from the backward tree
        goalpath.goalindex = -1;
        SimpleNode *pbackward = (SimpleNode*)iConnectedBackward;
        while(1) {
            vecnodes.push_back(pbackward);
            if(!pbackward->rrtparent) {
                goalpath.goalindex = pbackward->_userdata;
                break;
            }
            pbackward = pbackward->rrtparent;
        }

        BOOST_ASSERT( goalpath.goalindex >= 0 && goalpath.goalindex < (int)_vecGoalNodes.size() );
        _SimpleOptimizePath(vecnodes,10);
        const int dof = _parameters->GetDOF();
        goalpath.qall.resize(vecnodes.size()*dof);
        list<SimpleNode*>::iterator itprev = vecnodes.begin();
        list<SimpleNode*>::iterator itnext = itprev; itnext++;
        goalpath.length = 0;
        vector<dReal>::iterator itq = goalpath.qall.begin();
        std::copy((*itprev)->q, (*itprev)->q+dof, itq);
        itq += dof;
        vector<dReal> vivel(dof,1.0);
        for(size_t i = 0; i < vivel.size(); ++i) {
            if( _parameters->_vConfigVelocityLimit.at(i) != 0 ) {
                vivel[i] = 1/_parameters->_vConfigVelocityLimit.at(i);
            }
        }

        while(itnext != vecnodes.end()) {
            std::copy((*itnext)->q, (*itnext)->q+dof, itq);
            itprev=itnext;
            ++itnext;
            itq += dof;
        }

        // take distance scaled with respect to velocities with the first and last points only!
        // this is because rrt paths can initially be very complex but simplify down to something simpler.
        std::vector<dReal> vdiff(vecnodes.front()->q, vecnodes.front()->q+dof);
        _parameters->_diffstatefn(vdiff, VectorWrapper<dReal>(vecnodes.back()->q, dof));
        for(size_t i = 0; i < vdiff.size(); ++i) {
            goalpath.length += RaveFabs(vdiff.at(i))*vivel.at(i);
        }
    }

    virtual PlannerParametersConstPtr GetParameters() const {
        return _parameters;
    }

    virtual bool _DumpTreeCommand(std::ostream& os, std::istream& is) {
        std::string filename = RaveGetHomeDirectory() + string("/birrtdump.txt");
        getline(is, filename);
        boost::trim(filename);
        RAVELOG_VERBOSE(str(boost::format("dumping rrt tree to %s")%filename));
        ofstream f(filename.c_str());
        f << std::setprecision(std::numeric_limits<dReal>::digits10+1);
        _treeForward.DumpTree(f);
        _treeBackward.DumpTree(f);
        return true;
    }

protected:
    RRTParametersPtr _parameters;
    SpatialTree< SimpleNode > _treeBackward;
    dReal _fGoalBiasProb;
    std::vector< NodeBase* > _vecGoalNodes;
    size_t _nValidGoals; ///< num valid goals
};

class BasicRrtPlanner : public RrtPlanner<SimpleNode>
{
public:
    BasicRrtPlanner(EnvironmentBasePtr penv) : RrtPlanner<SimpleNode>(penv)
    {
        __description = "Rosen's Basic RRT planner";
        _fGoalBiasProb = dReal(0.05);
        _bOneStep = false;
    }
    virtual ~BasicRrtPlanner() {
    }

    bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr pparams)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new BasicRRTParameters());
        _parameters->copy(pparams);
        if( !RrtPlanner<SimpleNode>::_InitPlan(pbase,_parameters) ) {
            _parameters.reset();
            return false;
        }

        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);

        //read in all goals
        int goal_index = 0;
        vector<dReal> vgoal(_parameters->GetDOF());
        _vecGoals.resize(0);
        while(_parameters->vgoalconfig.size() > 0) {
            for(int i = 0; i < _parameters->GetDOF(); i++) {
                if(goal_index < (int)_parameters->vgoalconfig.size())
                    vgoal[i] = _parameters->vgoalconfig[goal_index];
                else {
                    RAVELOG_ERROR("BirrtPlanner::InitPlan - Error: goals are improperly specified:\n");
                    _parameters.reset();
                    return false;
                }
                goal_index++;
            }

            if( GetParameters()->CheckPathAllConstraints(vgoal,vgoal, std::vector<dReal>(), std::vector<dReal>(), 0, IT_OpenStart) == 0 ) {
                _vecGoals.push_back(vgoal);
            }
            else {
                RAVELOG_WARN("goal in collision\n");
            }

            if(goal_index == (int)_parameters->vgoalconfig.size()) {
                break;
            }
        }

        if(( _vecGoals.size() == 0) && !_parameters->_goalfn ) {
            RAVELOG_WARN("no goals or goal function specified\n");
            _parameters.reset();
            return false;
        }

        RAVELOG_DEBUG("RrtPlanner::InitPlan - RRT Planner Initialized\n");
        return true;
    }

    PlannerStatus PlanPath(TrajectoryBasePtr ptraj)
    {
        if(!_parameters) {
            RAVELOG_WARN("RrtPlanner::PlanPath - Error, planner not initialized\n");
            return PS_Failed;
        }

        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        uint32_t basetime = utils::GetMilliTime();

        NodeBase* lastnode = NULL;
        bool bSuccess = false;

        // the main planning loop
        PlannerParameters::StateSaver savestate(_parameters);
        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);

        PlannerAction callbackaction = PA_None;
        PlannerProgress progress;
        int iter = 0;
        _goalindex = -1;
        _startindex = -1;

        while(!bSuccess && iter < _parameters->_nMaxIterations) {
            iter++;

            if( !!_parameters->_samplegoalfn ) {
                vector<dReal> vgoal;
                if( _parameters->_samplegoalfn(vgoal) ) {
                    RAVELOG_VERBOSE("found goal\n");
                    _vecGoals.push_back(vgoal);
                }
            }
            if( !!_parameters->_sampleinitialfn ) {
                vector<dReal> vinitial;
                if( _parameters->_sampleinitialfn(vinitial) ) {
                    RAVELOG_VERBOSE("found initial\n");
                    _vecInitialNodes.push_back(_treeForward.InsertNode(NULL, vinitial, _vecInitialNodes.size()));
                }
            }

            if( (iter == 1 || RaveRandomFloat() < _fGoalBiasProb ) && _vecGoals.size() > 0 ) {
                _sampleConfig = _vecGoals[RaveRandomInt()%_vecGoals.size()];
            }
            else if( !_parameters->_samplefn(_sampleConfig) ) {
                continue;
            }

            // extend A
            ExtendType et = _treeForward.Extend(_sampleConfig, lastnode, _bOneStep);

            if( et == ET_Connected ) {
                FOREACH(itgoal, _vecGoals) {
                    if( _parameters->_distmetricfn(*itgoal, _treeForward.GetVectorConfig(lastnode)) < 2*_parameters->_fStepLength ) {
                        bSuccess = true;
                        _goalindex = (int)(itgoal-_vecGoals.begin());
                        RAVELOG_DEBUG(str(boost::format("found goal index: %d\n")%_goalindex));
                        break;
                    }
                }
            }

            // check the goal heuristic more often
            if(( et != ET_Failed) && !!_parameters->_goalfn ) {
                if( _parameters->_goalfn(_treeForward.GetVectorConfig(lastnode)) <= 1e-4f ) {
                    bSuccess = true;
                    _goalindex = -1;
                    RAVELOG_DEBUG("node at goal\n");
                    break;
                }
            }

            // check if reached any goals
            if( iter > _parameters->_nMaxIterations ) {
                RAVELOG_WARN("iterations exceeded %d\n", _parameters->_nMaxIterations);
                break;
            }

            progress._iteration = iter;
            callbackaction = _CallCallbacks(progress);
            if( callbackaction ==  PA_Interrupt ) {
                return PS_Interrupted;
            }
            else if( callbackaction == PA_ReturnWithAnySolution ) {
                if( bSuccess ) {
                    break;
                }
            }
        }

        if( !bSuccess ) {
            RAVELOG_DEBUG("plan failed, %fs\n",0.001f*(float)(utils::GetMilliTime()-basetime));
            return PS_Failed;
        }

        list<SimpleNode*> vecnodes;

        // add nodes from the forward tree
        SimpleNode* pforward = (SimpleNode*)lastnode;
        while(1) {
            vecnodes.push_front(pforward);
            if(!pforward->rrtparent) {
                break;
            }
            pforward = pforward->rrtparent;
        }

        _SimpleOptimizePath(vecnodes,10);
        if( ptraj->GetConfigurationSpecification().GetDOF() == 0 ) {
            ptraj->Init(_parameters->_configurationspecification);
        }
        FOREACH(itnode, vecnodes) {
            ptraj->Insert(ptraj->GetNumWaypoints(), VectorWrapper<dReal>((*itnode)->q, _parameters->GetDOF()), _parameters->_configurationspecification);
        }

        PlannerStatus status = _ProcessPostPlanners(_robot,ptraj);
        RAVELOG_DEBUG(str(boost::format("plan success, path=%d points in %fs\n")%ptraj->GetNumWaypoints()%((0.001f*(float)(utils::GetMilliTime()-basetime)))));
        return status;
    }

    virtual PlannerParametersConstPtr GetParameters() const {
        return _parameters;
    }

protected:
    boost::shared_ptr<BasicRRTParameters> _parameters;
    dReal _fGoalBiasProb;
    bool _bOneStep;
    std::vector< std::vector<dReal> > _vecGoals;
    int _nValidGoals; ///< num valid goals
};

class ExplorationPlanner : public RrtPlanner<SimpleNode>
{
public:
    class ExplorationParameters : public PlannerBase::PlannerParameters {
public:
        ExplorationParameters() : _fExploreProb(0), _nExpectedDataSize(100), _bProcessingExploration(false) {
            _vXMLParameters.push_back("exploreprob");
            _vXMLParameters.push_back("expectedsize");
        }

        dReal _fExploreProb;
        int _nExpectedDataSize;

protected:
        bool _bProcessingExploration;
        // save the extra data to XML
        virtual bool serialize(std::ostream& O) const
        {
            if( !PlannerParameters::serialize(O) ) {
                return false;
            }
            O << "<exploreprob>" << _fExploreProb << "</exploreprob>" << endl;
            O << "<expectedsize>" << _nExpectedDataSize << "</expectedsize>" << endl;
            return !!O;
        }

        ProcessElement startElement(const std::string& name, const AttributesList &atts)
        {
            if( _bProcessingExploration ) {
                return PE_Ignore;
            }
            switch( PlannerBase::PlannerParameters::startElement(name,atts) ) {
            case PE_Pass: break;
            case PE_Support: return PE_Support;
            case PE_Ignore: return PE_Ignore;
            }

            _bProcessingExploration = name=="exploreprob"||name=="expectedsize";
            return _bProcessingExploration ? PE_Support : PE_Pass;
        }

        // called at the end of every XML tag, _ss contains the data
        virtual bool endElement(const std::string& name)
        {
            // _ss is an internal stringstream that holds the data of the tag
            if( _bProcessingExploration ) {
                if( name == "exploreprob") {
                    _ss >> _fExploreProb;
                }
                else if( name == "expectedsize" ) {
                    _ss >> _nExpectedDataSize;
                }
                else {
                    RAVELOG_WARN(str(boost::format("unknown tag %s\n")%name));
                }
                _bProcessingExploration = false;
                return false;
            }

            // give a chance for the default parameters to get processed
            return PlannerParameters::endElement(name);
        }
    };

    ExplorationPlanner(EnvironmentBasePtr penv) : RrtPlanner<SimpleNode>(penv) {
        __description = ":Interface Author: Rosen Diankov\n\nRRT-based exploration planner";
    }
    virtual ~ExplorationPlanner() {
    }

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr pparams)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        _parameters.reset(new ExplorationParameters());
        _parameters->copy(pparams);
        if( !RrtPlanner<SimpleNode>::_InitPlan(pbase,_parameters) ) {
            _parameters.reset();
            return false;
        }
        RAVELOG_DEBUG("ExplorationPlanner::InitPlan - RRT Planner Initialized\n");
        return true;
    }

    virtual PlannerStatus PlanPath(TrajectoryBasePtr ptraj)
    {
        _goalindex = -1;
        _startindex = -1;
        if( !_parameters ) {
            return PS_Failed;
        }
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        vector<dReal> vSampleConfig;

        PlannerParameters::StateSaver savestate(_parameters);
        CollisionOptionsStateSaver optionstate(GetEnv()->GetCollisionChecker(),GetEnv()->GetCollisionChecker()->GetCollisionOptions()|CO_ActiveDOFs,false);

        int iter = 0;
        while(iter < _parameters->_nMaxIterations && _treeForward.GetNumNodes() < _parameters->_nExpectedDataSize ) {
            ++iter;

            if( RaveRandomFloat() < _parameters->_fExploreProb ) {
                // explore
                int inode = RaveRandomInt()%_treeForward.GetNumNodes();
                NodeBase* pnode = _treeForward.GetNodeFromIndex(inode);

                if( !_parameters->_sampleneighfn(vSampleConfig, _treeForward.GetVectorConfig(pnode), _parameters->_fStepLength) ) {
                    return PS_Failed;
                }
                if( GetParameters()->CheckPathAllConstraints(_treeForward.GetVectorConfig(pnode), vSampleConfig, std::vector<dReal>(), std::vector<dReal>(), 0, IT_OpenStart) == 0 ) {
                    _treeForward.InsertNode(pnode, vSampleConfig, 0);
                    GetEnv()->UpdatePublishedBodies();
                    RAVELOG_DEBUG_FORMAT("size %d", _treeForward.GetNumNodes());
                }
            }
            else {     // rrt extend
                if( !_parameters->_samplefn(vSampleConfig) ) {
                    continue;
                }
                NodeBase* plastnode;
                if( _treeForward.Extend(vSampleConfig, plastnode, true) == ET_Connected ) {
                    RAVELOG_DEBUG_FORMAT("size %d", _treeForward.GetNumNodes());
                }
            }
        }

        if( ptraj->GetConfigurationSpecification().GetDOF() == 0 ) {
            ptraj->Init(_parameters->_configurationspecification);
        }
        // save nodes to trajectory
        std::vector<NodeBase*> vnodes;
        _treeForward.GetNodesVector(vnodes);
        FOREACH(itnode, vnodes) {
            ptraj->Insert(ptraj->GetNumWaypoints(), _treeForward.GetVectorConfig(*itnode), _parameters->_configurationspecification);
        }
        return PS_HasSolution;
    }

    virtual PlannerParametersConstPtr GetParameters() const {
        return _parameters;
    }

private:
    boost::shared_ptr<ExplorationParameters> _parameters;

};

#ifdef RAVE_REGISTER_BOOST
#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()
BOOST_TYPEOF_REGISTER_TYPE(BirrtPlanner::GOALPATH)
#endif

#endif
