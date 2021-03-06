#include "mysimulator.h"
#include "states.h"
#include "LammpsWrappers/lammpserror.h"
#include "LammpsWrappers/simulatorcontrols/simulatorcontrol.h"
#include <library.h>
#include <atom.h>
#include <domain.h>
#include <compute_temp.h>
#include <update.h>
#include <modify.h>
#include <neighbor.h>
#include <neigh_list.h>
#include <string>
#include <sstream>
#include <SimVis/SphereData>
#include <QString>
#include <QQmlFile>
#include <QDir>
#include <iostream>
#include <fstream>
#include <memory>
#include <QFileInfo>
#include <QStandardPaths>
#include "LammpsWrappers/atoms.h"
#include "LammpsWrappers/modifiers/modifiers.h"
#include "LammpsWrappers/system.h"
#include "LammpsWrappers/computes.h"
#include "LammpsWrappers/fixes.h"
#include "LammpsWrappers/bonds.h"

using namespace std;

MyWorker::MyWorker() {
    m_sinceStart.start();
    m_elapsed.start();
    m_lammpsController.worker = this;
}

void MyWorker::setNeedsSynchronization(bool value)
{
    m_needsSynchronization = value;
}

bool MyWorker::needsSynchronization()
{
    return m_needsSynchronization;
}

AtomifySimulator::AtomifySimulator() :
    m_system(new System(this)),
    m_states(new States(this)),
    m_simulationSpeed(1)
{
    m_states->setupStates(*this);
    m_parser.setSimulator(this);
}

AtomifySimulator::~AtomifySimulator() { }

void AtomifySimulator::togglePause()
{
    if(m_states->paused()->active()) {
        emit unPaused();
    } else if(m_states->parsing()->active() || m_states->continued()->active()) {
        emit paused();
    } else if(m_states->finished()->active()) {
        emit continued();
    }
}

void AtomifySimulator::increaseSimulationSpeed()
{
    if(m_simulationSpeed < 100) {
        setSimulationSpeed(m_simulationSpeed + 1);
    }
}

void AtomifySimulator::decreaseSimulationSpeed()
{
    if(m_simulationSpeed > 1) {
        setSimulationSpeed(m_simulationSpeed - 1);
    }
}

System *AtomifySimulator::system() const
{
    return m_system;
}

States *AtomifySimulator::states() const
{
    return m_states;
}

QString AtomifySimulator::scriptFilePath() const
{
    return m_scriptFilePath;
}

QString AtomifySimulator::error() const
{
    return m_error;
}

void MyWorker::synchronizeSimulator(Simulator *simulator)
{
    QElapsedTimer t;
    t.start();
    AtomifySimulator *atomifySimulator = qobject_cast<AtomifySimulator*>(simulator);
    m_lammpsController.simulationSpeed = atomifySimulator->simulationSpeed();
    m_lammpsController.qmlThread = QThread::currentThread();

    atomifySimulator->syncCount += 1;
    States &states = *atomifySimulator->states();
    // Sync properties from lammps controller and back
    m_lammpsController.system = atomifySimulator->system();
    if(states.paused()->active()) {
        if(m_workerRenderingMutex.tryLock()) {
            atomifySimulator->system()->atoms()->synchronizeRenderer();
            m_workerRenderingMutex.unlock();
            m_reprocessRenderingData = true;
        }
        return;
    }

    if(states.continued()->active()) {
        m_lammpsController.doContinue = true;
        m_lammpsController.finished = false;
        emit atomifySimulator->parsing();
        return;
    }

    // If user pressed stop / restart, we should reset
    if(m_lammpsController.crashed) {
        m_lammpsController.crashed = false;
        m_lammpsController.finished = true;
        atomifySimulator->setError(m_lammpsController.errorMessage);
        emit atomifySimulator->crashed();
        return;
    }

    if(states.reset()->active() && !m_cancelPending) {
        m_cancelPending = true;
        return;
    }

    if(states.reset()->active() && m_lammpsController.finished) {
        m_lammpsController.stop();
        atomifySimulator->system()->synchronize(&m_lammpsController);
        atomifySimulator->system()->atoms()->reset();
        atomifySimulator->system()->atoms()->synchronizeRenderer();
        emit atomifySimulator->didReset();
        return;
    }

    if(m_lammpsController.finished && states.parsing()->active()) {
        emit atomifySimulator->finished();
        return;
    }

    if(m_cancelPending && m_lammpsController.didCancel) {
        m_cancelPending = false;
        m_lammpsController.stop();
        atomifySimulator->system()->reset();
        atomifySimulator->system()->synchronize(&m_lammpsController);
        atomifySimulator->system()->atoms()->reset();
        emit atomifySimulator->didReset();
        return;
    }

    // If we don't have a LAMMPS object, but we have a new script (aka in parsing state), create LAMMPS object
    if(!m_lammpsController.lammps() && states.parsing()->active()) {
        m_lammpsController.scriptFilePath = atomifySimulator->scriptFilePath();

        // Don't move camera if we rerun a simulation
        bool moveCamera = atomifySimulator->scriptFilePath() != atomifySimulator->lastScript();
        atomifySimulator->parser().parseFile(atomifySimulator->scriptFilePath(), moveCamera);
        atomifySimulator->setLastScript(atomifySimulator->scriptFilePath());
        m_lammpsController.start();
        return;
    }
    atomifySimulator->system()->synchronizeQML(&m_lammpsController);
    atomifySimulator->system()->atoms()->synchronizeRenderer();
    m_needsSynchronization = false;
}

void MyWorker::work()
{
    m_workCount += 1;
    m_lammpsController.run();
}

MyWorker *AtomifySimulator::createWorker()
{
    return new MyWorker();
}

QString AtomifySimulator::lastScript() const
{
    return m_lastScript;
}

void AtomifySimulator::setLastScript(const QString &lastScript)
{
    m_lastScript = lastScript;
}

CommandParser &AtomifySimulator::parser()
{
    return m_parser;
}

QVector3D AtomifySimulator::cameraPositionRequest() const
{
    return m_cameraPositionRequest;
}

QVector3D AtomifySimulator::cameraViewCenterRequest() const
{
    return m_cameraViewCenterRequest;
}

bool AtomifySimulator::welcomeSimulationRunning() const
{
    return m_welcomeSimulationRunning;
}

int AtomifySimulator::simulationSpeed() const
{
    return m_simulationSpeed;
}

void AtomifySimulator::setSimulationSpeed(int arg)
{
    if (m_simulationSpeed == arg)
        return;

    m_simulationSpeed = arg;
    emit simulationSpeedChanged(arg);
}

void AtomifySimulator::setSystem(System *system)
{
    if (m_system == system)
        return;

    m_system = system;
    emit systemChanged(system);
}

void AtomifySimulator::setStates(States *states)
{
    if (m_states == states)
        return;

    m_states = states;
    emit statesChanged(states);
}

void AtomifySimulator::setScriptFilePath(QString scriptFilePath)
{
    if(!m_scriptFilePath.isEmpty()) setWelcomeSimulationRunning(false);
    scriptFilePath.replace("file://", "");
    if (m_scriptFilePath == scriptFilePath)
        return;

    m_scriptFilePath = scriptFilePath;
    emit scriptFilePathChanged(scriptFilePath);
}

void AtomifySimulator::setError(QString error)
{
    if (m_error == error)
        return;

    m_error = error;
    emit errorChanged(error);
}

void AtomifySimulator::setCameraPositionRequest(QVector3D cameraPositionRequest)
{
    if (m_cameraPositionRequest == cameraPositionRequest)
        return;

    m_cameraPositionRequest = cameraPositionRequest;
}

void AtomifySimulator::setCameraViewCenterRequest(QVector3D cameraViewCenterRequest)
{
    if (m_cameraViewCenterRequest == cameraViewCenterRequest)
        return;

    m_cameraViewCenterRequest = cameraViewCenterRequest;
    emit cameraViewCenterRequestChanged(cameraViewCenterRequest);
}

void AtomifySimulator::setWelcomeSimulationRunning(bool welcomeSimulationRunning)
{
    if (m_welcomeSimulationRunning == welcomeSimulationRunning)
        return;

    m_welcomeSimulationRunning = welcomeSimulationRunning;
    emit welcomeSimulationRunningChanged(welcomeSimulationRunning);
}
