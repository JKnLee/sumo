/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.org/sumo
// Copyright (C) 2001-2023 German Aerospace Center (DLR) and others.
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// https://www.eclipse.org/legal/epl-2.0/
// This Source Code may also be made available under the following Secondary
// Licenses when the conditions for such availability set forth in the Eclipse
// Public License 2.0 are satisfied: GNU General Public License, version 2
// or later which is available at
// https://www.gnu.org/licenses/old-licenses/gpl-2.0-standalone.html
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later
/****************************************************************************/
/// @file    GNELoadThread.cpp
/// @author  Jakob Erdmann
/// @date    Feb 2011
///
// The thread that performs the loading of a Netedit-net (adapted from
// GUILoadThread)
/****************************************************************************/
#include <netbuild/NBFrame.h>
#include <netbuild/NBNetBuilder.h>
#include <netimport/NIFrame.h>
#include <netimport/NILoader.h>
#include <netwrite/NWFrame.h>
#include <utils/common/MsgRetrievingFunction.h>
#include <utils/common/SystemFrame.h>
#include <utils/gui/events/GUIEvent_Message.h>
#include <utils/options/OptionsCont.h>
#include <utils/options/OptionsIO.h>
#include <utils/xml/XMLSubSys.h>

#include "GNEApplicationWindow.h"
#include "GNEEvent_NetworkLoaded.h"
#include "GNELoadThread.h"
#include "GNENet.h"


// ===========================================================================
// member method definitions
// ===========================================================================
GNELoadThread::GNELoadThread(GNEApplicationWindow* applicationWindow, MFXSynchQue<GUIEvent*>& eq, FXEX::MFXThreadEvent& ev) :
    MFXSingleEventThread(applicationWindow->getApp(), applicationWindow),
    myApplicationWindow(applicationWindow),
    myEventQueue(eq),
    myEventThrow(ev) {
    myDebugRetriever = new MsgRetrievingFunction<GNELoadThread>(this, &GNELoadThread::retrieveMessage, MsgHandler::MsgType::MT_DEBUG);
    myGLDebugRetriever = new MsgRetrievingFunction<GNELoadThread>(this, &GNELoadThread::retrieveMessage, MsgHandler::MsgType::MT_GLDEBUG);
    myErrorRetriever = new MsgRetrievingFunction<GNELoadThread>(this, &GNELoadThread::retrieveMessage, MsgHandler::MsgType::MT_ERROR);
    myMessageRetriever = new MsgRetrievingFunction<GNELoadThread>(this, &GNELoadThread::retrieveMessage, MsgHandler::MsgType::MT_MESSAGE);
    myWarningRetriever = new MsgRetrievingFunction<GNELoadThread>(this, &GNELoadThread::retrieveMessage, MsgHandler::MsgType::MT_WARNING);
    MsgHandler::getErrorInstance()->addRetriever(myErrorRetriever);
}


GNELoadThread::~GNELoadThread() {
    delete myDebugRetriever;
    delete myGLDebugRetriever;
    delete myErrorRetriever;
    delete myMessageRetriever;
    delete myWarningRetriever;
}


FXint
GNELoadThread::run() {
    auto& neteditOptions = OptionsCont::getOptions();
    // register message callbacks
    MsgHandler::getMessageInstance()->addRetriever(myMessageRetriever);
    MsgHandler::getDebugInstance()->addRetriever(myDebugRetriever);
    MsgHandler::getGLDebugInstance()->addRetriever(myGLDebugRetriever);
    MsgHandler::getErrorInstance()->addRetriever(myErrorRetriever);
    MsgHandler::getWarningInstance()->addRetriever(myWarningRetriever);
    // flag for check if input is valid
    bool validInput = false;
    // declare network
    GNENet* net = nullptr;
    // declare loaded file
    std::string loadedFile;
    // check conditions
    if (neteditOptions.getBool("new")) {
        validInput = true;
    } else if (neteditOptions.getString("net-file").size() > 0) {
        validInput = true;
        // add it into recent networks
        myApplicationWindow->getMenuBarFile().myRecentNetworks.appendFile(loadedFile.c_str());
    } else if (neteditOptions.getString("sumocfg-file").size() > 0) {
        // set sumo config as loaded file
        loadedFile = neteditOptions.getString("sumocfg-file");
        // declare parser for sumo config file
        GNEApplicationWindowHelper::GNESUMOConfigHandler confighandler(myApplicationWindow->getSUMOOptions(), loadedFile);
        // if there is an error loading sumo config, stop
        if (confighandler.loadSUMOConfig()) {
            validInput = true;
            // add it into recent configs
            myApplicationWindow->getMenuBarFile().myRecentConfigs.appendFile(loadedFile.c_str());
        } else {
            WRITE_ERROR("Loading of sumo config file '" + loadedFile + "' failed.");
            submitEndAndCleanup(net, loadedFile);
            return 0;
        }
    } else if (neteditOptions.getString("configuration-file").size() > 0) {
        // set netedit config as loaded file
        loadedFile = neteditOptions.getString("configuration-file");
        // declare parser for netedit config file
        GNEApplicationWindowHelper::GNENETEDITConfigHandler confighandler(loadedFile);
        // if there is an error loading sumo config, stop
        if (confighandler.loadNETEDITConfig()) {
            validInput = true;
            // add it into recent configs
            myApplicationWindow->getMenuBarFile().myRecentConfigs.appendFile(loadedFile.c_str());
        } else {
            WRITE_ERROR("Loading of netedit config file '" + loadedFile + "' failed.");
            submitEndAndCleanup(net, loadedFile);
            return 0;
        }
    } else if (loadConsoleOptions()) {
        validInput = true;
    }
    // check input
    if (!validInput) {    
        WRITE_ERROR("Invalid input options for GNELoadThread::run()");
        submitEndAndCleanup(net, loadedFile);
        return 0;
    }
    // update aggregate warnings
    if (neteditOptions.isDefault("aggregate-warnings")) {
        neteditOptions.setDefault("aggregate-warnings", "5");
    }
    // init output options
    MsgHandler::initOutputOptions();
    // if there is an error checking options, stop
    if (!(NIFrame::checkOptions() && NBFrame::checkOptions() && NWFrame::checkOptions() && SystemFrame::checkOptions())) {
        // options are not valid
        WRITE_ERROR(TL("Invalid Options. Nothing loaded"));
        submitEndAndCleanup(net, loadedFile);
        return 0;
    }
    // clear message instances
    MsgHandler::getGLDebugInstance()->clear();
    MsgHandler::getDebugInstance()->clear();
    MsgHandler::getErrorInstance()->clear();
    MsgHandler::getWarningInstance()->clear();
    MsgHandler::getMessageInstance()->clear();
    // init global random seed
    RandHelper::initRandGlobal();
    // check if geo projection can be inited
    if (!GeoConvHelper::init(neteditOptions)) {
        WRITE_ERROR(TL("Could not build projection!"));
        submitEndAndCleanup(net, loadedFile);
        return 0;
    }
    // set validation
    XMLSubSys::setValidation(neteditOptions.getString("xml-validation"), neteditOptions.getString("xml-validation.net"), neteditOptions.getString("xml-validation.routes"));
    // check if Debug has to be enabled
    MsgHandler::enableDebugMessages(neteditOptions.getBool("gui-testing-debug"));
    // check if GL Debug has to be enabled
    MsgHandler::enableDebugGLMessages(neteditOptions.getBool("gui-testing-debug-gl"));
    // create netBuilder (will be destroyed in GNENet destructor)
    NBNetBuilder* netBuilder = new NBNetBuilder();
    // apply netedit options in netBuilder. In this options we have all information for building network
    netBuilder->applyOptions(neteditOptions);
    // check if create a new net
    if (neteditOptions.getBool("new")) {
        // create new network
        net = new GNENet(netBuilder);
    } else {
        // declare net loader
        NILoader nl(*netBuilder);
        try {
            // try to load network using netedit options
            nl.load(neteditOptions);
            if (true) {    // CHECK
                // make coordinate conversion usable before first netBuilder->compute()
                GeoConvHelper::computeFinal();
            } else {
                WRITE_MESSAGE(TL("Performing initial computation ...\n"));
                // perform one-time processing (i.e. edge removal)
                netBuilder->compute(neteditOptions);
                // @todo remove one-time processing options!
            }
            // check if ignore errors
            if (neteditOptions.getBool("ignore-errors")) {
                MsgHandler::getErrorInstance()->clear();
            }
            // check whether any errors occurred
            if (MsgHandler::getErrorInstance()->wasInformed()) {
                throw ProcessError();
            } else {
                // now create net with al information loaded in net builder
                net = new GNENet(netBuilder);
                // chek if change traffic direction
                if (neteditOptions.getBool("lefthand")) {
                    // force initial geometry computation without volatile options because the net will look strange otherwise
                    net->computeAndUpdate(neteditOptions, false);
                }
                // check if add prefixes
                if (neteditOptions.getString("prefix").size() > 0) {
                    // change prefixes in attributeCarriers
                    net->getAttributeCarriers()->addPrefixToEdges(neteditOptions.getString("prefix"));
                    net->getAttributeCarriers()->addPrefixToJunctions(neteditOptions.getString("prefix"));
                    // change prefix in containers
                    net->getNetBuilder()->getNodeCont().addPrefix(neteditOptions.getString("prefix"));
                    net->getNetBuilder()->getEdgeCont().addPrefix(neteditOptions.getString("prefix"));
                }
            }
        } catch (ProcessError& e) {
            if (std::string(e.what()) != std::string("Process Error") && std::string(e.what()) != std::string("")) {
                WRITE_ERROR(e.what());
            }
            WRITE_ERROR(TL("Failed to build network."));
            // check if delete network
            if (net != nullptr) {
                delete net;
                net = nullptr;
            } else {
                // GNENet not created, then delete netBuilder
                delete netBuilder;
            }
        } catch (std::exception& e) {
            WRITE_ERROR(e.what());
            // check if delete network
            if (net != nullptr) {
                delete net;
                net = nullptr;
            } else {
                // GNENet not created, then delete netBuilder
                delete netBuilder;
            }
        }
    }
    // only a single setting file is supported
    submitEndAndCleanup(net, loadedFile, neteditOptions.getString("gui-settings-file"), neteditOptions.getBool("registry-viewport"));
    return 0;
}



void
GNELoadThread::submitEndAndCleanup(GNENet* net, const std::string& loadedFile, const std::string& guiSettingsFile, const bool viewportFromRegistry) {
    // remove message callbacks
    MsgHandler::getDebugInstance()->removeRetriever(myDebugRetriever);
    MsgHandler::getGLDebugInstance()->removeRetriever(myGLDebugRetriever);
    MsgHandler::getErrorInstance()->removeRetriever(myErrorRetriever);
    MsgHandler::getWarningInstance()->removeRetriever(myWarningRetriever);
    MsgHandler::getMessageInstance()->removeRetriever(myMessageRetriever);
    // inform parent about the process
    myEventQueue.push_back(new GNEEvent_NetworkLoaded(net, loadedFile, guiSettingsFile, viewportFromRegistry));
    myEventThrow.signal();
}


void
GNELoadThread::fillOptions(OptionsCont& neteditOptions) {
    neteditOptions.clear();
    neteditOptions.addCallExample("--new", "start plain GUI with empty net");
    neteditOptions.addCallExample("-s <SUMO_NET>", "open a SUMO network");
    neteditOptions.addCallExample("-c <CONFIGURATION>", "open a configuration file (NETEDIT o NETCONVERT config)");
    neteditOptions.addCallExample("-sumocfg-file <CONFIGURATION>", "open a SUMO config file");

    SystemFrame::addConfigurationOptions(neteditOptions); // this subtopic is filled here, too
    neteditOptions.addOptionSubTopic("Input");
    neteditOptions.addOptionSubTopic("Output");
    GeoConvHelper::addProjectionOptions(neteditOptions);
    neteditOptions.addOptionSubTopic("Processing");
    neteditOptions.addOptionSubTopic("Building Defaults");
    neteditOptions.addOptionSubTopic("TLS Building");
    neteditOptions.addOptionSubTopic("Ramp Guessing");
    neteditOptions.addOptionSubTopic("Edge Removal");
    neteditOptions.addOptionSubTopic("Unregulated Nodes");
    neteditOptions.addOptionSubTopic("Junctions");
    neteditOptions.addOptionSubTopic("Pedestrian");
    neteditOptions.addOptionSubTopic("Bicycle");
    neteditOptions.addOptionSubTopic("Railway");
    neteditOptions.addOptionSubTopic("Formats");
    neteditOptions.addOptionSubTopic("Netedit");
    neteditOptions.addOptionSubTopic("Visualisation");
    neteditOptions.addOptionSubTopic("Time");

    // TOPIC: Input

    neteditOptions.doRegister("sumocfg-file", new Option_FileName());
    neteditOptions.addSynonyme("sumocfg-file", "sumocfg");
    neteditOptions.addDescription("sumocfg-file", "Input", "load sumo config");

    neteditOptions.doRegister("additional-files", 'a', new Option_FileName());
    neteditOptions.addSynonyme("additional-files", "additional");
    neteditOptions.addDescription("additional-files", "Input", "load additional and shapes descriptions from FILE(s)");

    neteditOptions.doRegister("route-files", 'r', new Option_FileName());
    neteditOptions.addSynonyme("route-files", "routes");
    neteditOptions.addDescription("route-files", "Input", "load demand elements descriptions from FILE(s)");

    neteditOptions.doRegister("data-files", 'd', new Option_FileName());
    neteditOptions.addSynonyme("data-files", "data");
    neteditOptions.addDescription("data-files", "Input", "load data elements descriptions from FILE(s)");

    neteditOptions.doRegister("meandata-files", 'm', new Option_FileName());
    neteditOptions.addSynonyme("meandata-files", "meandata");
    neteditOptions.addDescription("meandata-files", "Input", "load meanData descriptions from FILE(s)");

    // TOPIC: Output

    neteditOptions.doRegister("tls-file", new Option_String());
    neteditOptions.addDescription("tls-file", "Output", "file in which TLS Programs must be saved");

    neteditOptions.doRegister("edgetypes-file", new Option_String());
    neteditOptions.addDescription("edgetypes-file", "Output", "file in which edgeTypes must be saved");

    // TOPIC: Netedit

    neteditOptions.doRegister("new-network", new Option_Bool(false));
    neteditOptions.addSynonyme("new-network", "new");
    neteditOptions.addDescription("new-network", "Netedit", "start netedit with a new network");

    // network prefixes

    neteditOptions.doRegister("node-prefix", new Option_String("J"));
    neteditOptions.addDescription("node-prefix", "Netedit", "prefix for node naming");

    neteditOptions.doRegister("edge-prefix", new Option_String("E"));
    neteditOptions.addDescription("edge-prefix", "Netedit", "prefix for edge naming");

    neteditOptions.doRegister("edge-infix", new Option_String(""));
    neteditOptions.addDescription("edge-infix", "Netedit", "enable edge-infix (<fromNodeID><infix><toNodeID>)");

    // additional prefixes

    neteditOptions.doRegister("busStop-prefix", new Option_String("bs"));
    neteditOptions.addDescription("busStop-prefix", "Netedit", "prefix for busStop naming");

    neteditOptions.doRegister("trainStop-prefix", new Option_String("ts"));
    neteditOptions.addDescription("trainStop-prefix", "Netedit", "prefix for trainStop naming");

    neteditOptions.doRegister("containerStop-prefix", new Option_String("ct"));
    neteditOptions.addDescription("containerStop-prefix", "Netedit", "prefix for containerStop naming");

    neteditOptions.doRegister("chargingStation-prefix", new Option_String("cs"));
    neteditOptions.addDescription("chargingStation-prefix", "Netedit", "prefix for chargingStation naming");

    neteditOptions.doRegister("parkingArea-prefix", new Option_String("pa"));
    neteditOptions.addDescription("parkingArea-prefix", "Netedit", "prefix for parkingArea naming");

    neteditOptions.doRegister("e1Detector-prefix", new Option_String("e1"));
    neteditOptions.addDescription("e1Detector-prefix", "Netedit", "prefix for e1Detector naming");

    neteditOptions.doRegister("e2Detector-prefix", new Option_String("e2"));
    neteditOptions.addDescription("e2Detector-prefix", "Netedit", "prefix for e2Detector naming");

    neteditOptions.doRegister("e3Detector-prefix", new Option_String("e3"));
    neteditOptions.addDescription("e3Detector-prefix", "Netedit", "prefix for e3Detector naming");

    neteditOptions.doRegister("e1InstantDetector-prefix", new Option_String("e1i"));
    neteditOptions.addDescription("e1InstantDetector-prefix", "Netedit", "prefix for e1InstantDetector naming");

    neteditOptions.doRegister("rerouter-prefix", new Option_String("rr"));
    neteditOptions.addDescription("rerouter-prefix", "Netedit", "prefix for rerouter naming");

    neteditOptions.doRegister("calibrator-prefix", new Option_String("ca"));
    neteditOptions.addDescription("calibrator-prefix", "Netedit", "prefix for calibrator naming");

    neteditOptions.doRegister("routeProbe-prefix", new Option_String("rp"));
    neteditOptions.addDescription("routeProbe-prefix", "Netedit", "prefix for routeProbe naming");

    neteditOptions.doRegister("vss-prefix", new Option_String("vs"));
    neteditOptions.addDescription("vss-prefix", "Netedit", "prefix for variable speed sign naming");

    neteditOptions.doRegister("tractionSubstation-prefix", new Option_String("tr"));
    neteditOptions.addDescription("tractionSubstation-prefix", "Netedit", "prefix for traction substation naming");

    neteditOptions.doRegister("overheadWire-prefix", new Option_String("ow"));
    neteditOptions.addDescription("overheadWire-prefix", "Netedit", "prefix for overhead wire naming");

    neteditOptions.doRegister("polygon-prefix", new Option_String("po"));
    neteditOptions.addDescription("polygon-prefix", "Netedit", "prefix for polygon naming");

    neteditOptions.doRegister("poi-prefix", new Option_String("poi"));
    neteditOptions.addDescription("poi-prefix", "Netedit", "prefix for poi naming");

    // demand prefixes

    neteditOptions.doRegister("route-prefix", new Option_String("r"));
    neteditOptions.addDescription("route-prefix", "Netedit", "prefix for route naming");

    neteditOptions.doRegister("vType-prefix", new Option_String("t"));
    neteditOptions.addDescription("vType-prefix", "Netedit", "prefix for vType naming");

    neteditOptions.doRegister("vehicle-prefix", new Option_String("v"));
    neteditOptions.addDescription("vehicle-prefix", "Netedit", "prefix for vehicle naming");

    neteditOptions.doRegister("trip-prefix", new Option_String("t"));
    neteditOptions.addDescription("trip-prefix", "Netedit", "prefix for trip naming");

    neteditOptions.doRegister("flow-prefix", new Option_String("f"));
    neteditOptions.addDescription("flow-prefix", "Netedit", "prefix for flow naming");

    neteditOptions.doRegister("person-prefix", new Option_String("p"));
    neteditOptions.addDescription("person-prefix", "Netedit", "prefix for person naming");

    neteditOptions.doRegister("personflow-prefix", new Option_String("pf"));
    neteditOptions.addDescription("personflow-prefix", "Netedit", "prefix for personFlow naming");

    neteditOptions.doRegister("container-prefix", new Option_String("c"));
    neteditOptions.addDescription("container-prefix", "Netedit", "prefix for container naming");

    neteditOptions.doRegister("containerflow-prefix", new Option_String("cf"));
    neteditOptions.addDescription("containerflow-prefix", "Netedit", "prefix for containerFlow naming");

    // data prefixes

    // mean data prefixes

    neteditOptions.doRegister("meanDataEdge-prefix", new Option_String("ed"));
    neteditOptions.addDescription("meanDataEdge-prefix", "Netedit", "prefix for meanDataEdge naming");

    neteditOptions.doRegister("meanDataLane-prefix", new Option_String("ld"));
    neteditOptions.addDescription("meanDataLane-prefix", "Netedit", "prefix for meanDataLane naming");

    // TOPIC: Visualisation

    // textures

    neteditOptions.doRegister("disable-laneIcons", new Option_Bool(false));
    neteditOptions.addDescription("disable-laneIcons", "Visualisation", "Disable icons of special lanes");

    neteditOptions.doRegister("disable-textures", 'T', new Option_Bool(false)); // !!!
    neteditOptions.addDescription("disable-textures", "Visualisation", "");

    neteditOptions.doRegister("gui-settings-file", 'g', new Option_FileName());
    neteditOptions.addDescription("gui-settings-file", "Visualisation", "Load visualisation settings from FILE");

    // windows position

    neteditOptions.doRegister("registry-viewport", new Option_Bool(false));
    neteditOptions.addDescription("registry-viewport", "Visualisation", "Load current viewport from registry");

    neteditOptions.doRegister("window-size", new Option_StringVector());
    neteditOptions.addDescription("window-size", "Visualisation", "Create initial window with the given x,y size");

    neteditOptions.doRegister("window-pos", new Option_StringVector());
    neteditOptions.addDescription("window-pos", "Visualisation", "Create initial window at the given x,y position");

    // testing

    neteditOptions.doRegister("gui-testing", new Option_Bool(false));
    neteditOptions.addDescription("gui-testing", "Visualisation", "Enable overlay for screen recognition");

    neteditOptions.doRegister("gui-testing-debug", new Option_Bool(false));
    neteditOptions.addDescription("gui-testing-debug", "Visualisation", "Enable output messages during GUI-Testing");

    neteditOptions.doRegister("gui-testing-debug-gl", new Option_Bool(false));
    neteditOptions.addDescription("gui-testing-debug-gl", "Visualisation", "Enable output messages during GUI-Testing specific of gl functions");

    neteditOptions.doRegister("gui-testing.setting-output", new Option_FileName());
    neteditOptions.addDescription("gui-testing.setting-output", "Visualisation", "Save gui settings in the given settings-output file");

    // TOPIC: Time

    // register the simulation settings (needed for GNERouteHandler)

    neteditOptions.doRegister("begin", new Option_String("0", "TIME"));
    neteditOptions.addDescription("begin", "Time", "Defines the begin time in seconds; The simulation starts at this time");

    neteditOptions.doRegister("end", new Option_String("-1", "TIME"));
    neteditOptions.addDescription("end", "Time", "Defines the end time in seconds; The simulation ends at this time");

    neteditOptions.doRegister("default.action-step-length", new Option_Float(0.0));
    neteditOptions.addDescription("default.action-step-length", "Processing", "Length of the default interval length between action points for the car-following and lane-change models (in seconds). If not specified, the simulation step-length is used per default. Vehicle- or VType-specific settings override the default. Must be a multiple of the simulation step-length.");

    neteditOptions.doRegister("default.speeddev", new Option_Float(-1));
    neteditOptions.addDescription("default.speeddev", "Processing", "Select default speed deviation. A negative value implies vClass specific defaults (0.1 for the default passenger class");

    // fill rest of options

    NIFrame::fillOptions(true);
    NBFrame::fillOptions(false);
    NWFrame::fillOptions(false);
    RandHelper::insertRandOptions();
}


void
GNELoadThread::setDefaultOptions(OptionsCont& neteditOptions) {
    neteditOptions.resetWritable();
    neteditOptions.set("offset.disable-normalization", "true"); // preserve the given network as far as possible
    neteditOptions.set("no-turnarounds", "true"); // otherwise it is impossible to manually removed turn-arounds
}


bool
GNELoadThread::loadConsoleOptions() {
    // only loaded once
    if (myApplicationWindow->consoleOptionsLoaded()) {
        // get netedit options
        auto& neteditOptions = OptionsCont::getOptions();
        // fill (reset) all options
        fillOptions(neteditOptions);
        // set default options defined in GNELoadThread::setDefaultOptions(...)
        setDefaultOptions(neteditOptions);
        try {
            // set all values writable, because certain attributes already setted can be updated through console
            OptionsCont::getOptions().resetWritable();
            // load options from console
            OptionsIO::getOptions();
            return true;
        } catch (ProcessError& e) {
            if (std::string(e.what()) != std::string("Process Error") && std::string(e.what()) != std::string("")) {
                WRITE_ERROR(e.what());
            }
            WRITE_ERROR(TL("Failed to reset options."));
            return false;
        }
    } else {
        return false;
    }
}


void
GNELoadThread::newNetwork() {
    auto& neteditOptions = OptionsCont::getOptions();
    // reset netedit options
    fillOptions(neteditOptions);
    setDefaultOptions(neteditOptions);
    // enable option "new"
    neteditOptions.resetWritable();
    neteditOptions.set("new", "true");
    // start thread
    start();
}


void
GNELoadThread::loadNetworkOrConfig() {
    // start thread
    start();
}


void
GNELoadThread::retrieveMessage(const MsgHandler::MsgType type, const std::string& msg) {
    myEventQueue.push_back(new GUIEvent_Message(type, msg));
    myEventThrow.signal();
}

/****************************************************************************/
