// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

///
/// \file     TrendingTaskTPC.cxx
/// \author   Marcel Lesch
/// \author   Cindy Mordasini
/// \author   Based on the work from Piotr Konopka
///

#include "QualityControl/DatabaseInterface.h"
#include "QualityControl/MonitorObject.h"
#include "QualityControl/RootClassFactory.h"
#include "QualityControl/QcInfoLogger.h"
#include "TPC/ReductorTPC.h"
#include "TPC/TH1ReductorTPC.h"
#include "TPC/TH2ReductorTPC.h"
#include "TPC/SliceInfo.h"
#include "TPC/TrendingTaskTPC.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/algorithm/string.hpp>
#include <string>
#include <TAxis.h>
#include <TCanvas.h>
#include <TDatime.h>
#include <TFile.h>
#include <TGraph.h>
#include <TGraphErrors.h>
#include <TH1.h>
#include <TPaveText.h>
#include <TPoint.h>
#include <TTreeReader.h>
#include <TTreeReaderValue.h>
#include <TTreeReaderArray.h>

using namespace o2::quality_control;
using namespace o2::quality_control::core;
using namespace o2::quality_control::postprocessing;
using namespace o2::quality_control_modules::tpc;

void TrendingTaskTPC::configure(std::string name,
                                const boost::property_tree::ptree& config)
{
  mConfig = TrendingTaskConfigTPC(name, config);
}

void TrendingTaskTPC::initialize(Trigger, framework::ServiceRegistry&)
{
  // Prepare the data structure of the trending TTree.
  mTrend = std::make_unique<TTree>(); // TODO: retrieve last TTree, so we
                                      // continue trending. Maybe do it optionally?
  mTrend->SetName(PostProcessingInterface::getName().c_str());
  mTrend->Branch("meta", &mMetaData, "runNumber/I");
  mTrend->Branch("time", &mTime);

  for (const auto& source : mConfig.dataSources) {
    mSources[source.name] = std::vector<SliceInfo>();

    std::unique_ptr<ReductorTPC> reductor(root_class_factory::create<ReductorTPC>(
      source.moduleName, source.reductorName));
    mTrend->Branch(source.name.c_str(), &mSources[source.name]);
    mReductors[source.name] = std::move(reductor);
  }

  getObjectsManager()->startPublishing(mTrend.get());
}

// TODO: see if OptimizeBaskets() indeed helps after some time.
void TrendingTaskTPC::update(Trigger t, framework::ServiceRegistry& services)
{
  auto& qcdb = services.get<repository::DatabaseInterface>();
  trendValues(t.timestamp, qcdb);
  generatePlots();
}

void TrendingTaskTPC::finalize(Trigger, framework::ServiceRegistry&)
{
  generatePlots();
}

void TrendingTaskTPC::trendValues(uint64_t timestamp,
                                  repository::DatabaseInterface& qcdb)
{
  mTime = timestamp / 1000; // ROOT expects seconds since epoch.
  mMetaData.runNumber = -1;

  for (auto& dataSource : mConfig.dataSources) {
    if (dataSource.type == "repository") {
      auto mo = qcdb.retrieveMO(dataSource.path, dataSource.name, timestamp);
      TObject* obj = mo ? mo->getObject() : nullptr;
      if (obj) {
        mReductors[dataSource.name]->update(obj, mSources[dataSource.name],
                                            dataSource.axisDivision);

        // Number of pads depending if input canvas or single sliced histogram.
        if (auto canvas = dynamic_cast<TCanvas*>(obj)) { // Case with input canvas.
          mNumberPads = static_cast<TList*>(canvas->GetListOfPrimitives())->GetEntries();
        } else if ((int)dataSource.axisDivision[0].size() > 1) { // Case with sliced single histogram.
          int axisSize = (int)dataSource.axisDivision.size();
          int innerAxisSize = (int)dataSource.axisDivision[0].size() - 1;
          mNumberPads = axisSize * innerAxisSize;
        } else {
          mNumberPads = 1;
        }
      }

    } else if (dataSource.type == "repository-quality") {
      if (auto qo = qcdb.retrieveQO(dataSource.path + "/" + dataSource.name, timestamp)) {
        mReductors[dataSource.name]->update(qo.get(), mSources[dataSource.name],
                                            dataSource.axisDivision);
      }
    } else {
      ILOG(Error, Support) << "Data source '" << dataSource.type << "' unknown." << ENDM;
    }
  }

  mTrend->Fill();
  ILOG(Info, Support) << "'mTrend' has been filled with the current entry." << ENDM;
}

void TrendingTaskTPC::generatePlots()
{
  if (mTrend->GetEntries() < 1) {
    ILOG(Info, Support) << "No entries in the trend so far, no plot generated." << ENDM;
    return;
  }

  ILOG(Info, Support) << "Generating " << mConfig.plots.size() << " plots." << ENDM;
  for (const auto& plot : mConfig.plots) {
    // Delete the existing plots before regenerating them.
    if (mPlots.count(plot.name)) {
      getObjectsManager()->stopPublishing(plot.name);
      delete mPlots[plot.name];
    }

    // Draw the trending on a new canvas.
    TCanvas* c = new TCanvas();
    c->SetName(plot.name.c_str());
    c->SetTitle(plot.title.c_str());
    drawCanvas(c, plot.varexp, plot.selection, plot.option, plot.graphErrors, plot.name);

    // Postprocess each pad (titles, axes, flushing buffers).
    for (int p = 1; p <= mNumberPads; p++) {
      c->cd(p);
      if (auto histo = dynamic_cast<TGraph*>(c->cd(p)->GetPrimitive("Graph"))) {
        /*
                // Set a centered, nicely formatted title.
                ///if (auto title = static_cast<TPaveText*>(c->GetPrimitive("title"))) {
                if (auto title = dynamic_cast<TPaveText*>(histo->GetTitle())) {
                  title->SetBBoxCenterX(c->GetBBoxCenter().fX);
                  title->Draw();
                } else {
                  ILOG(Error, Devel) << "Could not get the title 'TPaveText' of the plot '"
                    << plot.name << "'." << ENDM;
                }*/
        /// LOKI

        // Set the user-defined range on the y axis if needed.
        if (!plot.graphYRange.empty()) {
          std::size_t posDivider = plot.graphYRange.find(":");
          std::string yMinString(plot.graphYRange.substr(0, posDivider));
          std::string yMaxString(plot.graphYRange.substr(posDivider + 1));

          float yMin = std::stof(yMinString);
          float yMax = std::stof(yMaxString);
          histo->SetMinimum(yMin);
          histo->SetMaximum(yMax);
        }

        // Configure the time for the x axis.
        if (plot.varexp.find(":time") != std::string::npos) {
          histo->GetXaxis()->SetTimeDisplay(1);
          histo->GetXaxis()->SetNdivisions(505);
          histo->GetXaxis()->SetTimeOffset(0.0);
          histo->GetXaxis()->SetTimeFormat("%Y-%m-%d %H:%M");
        }

        // Manually empty the buffers before visualising the plot.
        // histo->BufferEmpty(); // TBD: Should we keep it or not?
      } else {
        ILOG(Error, Devel) << "Could not get the 'Graph' of the plot '"
                           << plot.name << "'." << ENDM;
      }
    }

    mPlots[plot.name] = c;
    getObjectsManager()->startPublishing(c);
  }
}

void TrendingTaskTPC::drawCanvas(TCanvas* thisCanvas, const std::string& var,
                                 const std::string& sel, const std::string& opt, const std::string& err,
                                 const std::string& name)
{
  // Divide the canvas into the correct number of pads.
  thisCanvas->DivideSquare(mNumberPads);

  // Determine the order of the plot (1 - histo, 2 - graph, ...)
  const size_t plotOrder = std::count(var.begin(), var.end(), ':') + 1;

  // Delete the graph errors after the plot is saved.
  // Unfortunately the canvas does not take its ownership.
  TGraph* graphPad = nullptr;
  TGraphErrors* graphErrors = nullptr;

  // Prepare the strings for the dataSource and its trending quantity.
  std::size_t posEndVar = var.find(".");  // Find the end of the dataSource.
  std::size_t posEndType = var.find(":"); // Find the end of the quantity.
  std::string varName(var.substr(0, posEndVar));
  std::string typeName(var.substr(posEndVar + 1, posEndType - posEndVar - 1));

  std::size_t posEndType_err = err.find(":"); // Find the end of the error.
  std::string errXName(err.substr(posEndType_err + 1));
  std::string errYName(err.substr(0, posEndType_err));

  // Setup the tree reader with the needed values.
  TTreeReader myReader(mTrend.get());
  TTreeReaderValue<UInt_t> RetrieveTime(myReader, "time");
  TTreeReaderValue<std::vector<SliceInfo>> DataRetrieveVector(myReader, varName.data());

  int iEvent = 0;
  const int nEvents = mTrend->GetEntriesFast();
  const int NuPa = mNumberPads;
  double TimeStorage[nEvents];
  double DataStorage[NuPa][nEvents];
  double ErrorX[NuPa][nEvents];
  double ErrorY[NuPa][nEvents];
  ILOG(Info, Support) << "Total number of entries: " << nEvents << ENDM;

  while (myReader.Next()) {
    if (iEvent >= nEvents) {
      ILOG(Error, Support) << "Something went wrong, the reader is going too far." << ENDM;
      break;
    }

    TimeStorage[iEvent] = (double)(*RetrieveTime);

    for (int p = 0; p < NuPa; p++) {
      DataStorage[p][iEvent] = (DataRetrieveVector->at(p)).RetrieveValue(typeName);

      if (!err.empty()) {
        ErrorX[p][iEvent] = (DataRetrieveVector->at(p)).RetrieveValue(errXName);
        ErrorY[p][iEvent] = (DataRetrieveVector->at(p)).RetrieveValue(errYName);
      } else {
        ErrorX[p][iEvent] = 0.;
        ErrorY[p][iEvent] = 0.;
      }
    }
    iEvent++;
  }

  // Fill the graph(errors) to be published.
  for (int p = 0; p < (mNumberPads); p++) {
    thisCanvas->cd(p + 1);
    graphPad = new TGraph(nEvents, TimeStorage, DataStorage[p]);
    graphPad->SetTitle(Form("%s Pad %d", name.c_str(), p + 1));
    graphPad->Draw(opt.data());
    //// LOKI: Set title of the graph here.

    // Draw errors if they are specified.
    if (!err.empty()) {
      if (plotOrder != 2) {
        ILOG(Error, Support) << "Non empty graphErrors seen for the plot '" << name
                             << "', which is not a graph, ignoring." << ENDM;
      } else {
        graphPad->Draw("goff");
        graphErrors = new TGraphErrors(nEvents, TimeStorage, DataStorage[p], ErrorX[p], ErrorY[p]);

        // We draw on the same plot as the main graph, but only error bars.
        graphErrors->Draw("SAME E");

        // We try to convince ROOT to delete graphErrors together with the rest of the canvas.
        if (auto* pad = thisCanvas->GetPad(p + 1)) {
          if (auto* primitives = pad->GetListOfPrimitives()) {
            primitives->Add(graphErrors);
          }
        }
      }
    }

    ILOG(Info, Support) << "Pad " << p << " has been drawn." << ENDM;
  }
}