/* InterSpec: an application to analyze spectral gamma radiation data.
 
 Copyright 2018 National Technology & Engineering Solutions of Sandia, LLC
 (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the U.S.
 Government retains certain rights in this software.
 For questions contact William Johnson via email at wcjohns@sandia.gov, or
 alternative emails of interspec@sandia.gov.
 
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.
 
 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "InterSpec_config.h"

#include <string>
#include <iostream>

#include "SpecUtils/SpecFile.h"
#include "SpecUtils/StringAlgo.h"

#include "InterSpec/InterSpec.h"
#include "InterSpec/PeakModel.h"
#include "InterSpec/InterSpecApp.h"
#include "InterSpec/WarningWidget.h"
#include "InterSpec/UndoRedoManager.h"


using namespace std;


bool UndoRedoManager::SpecKeyLess::operator()( const spec_key_t &lhs, const spec_key_t &rhs ) const noexcept
{
    const std::weak_ptr<SpecMeas> &lptr = std::get<0>(lhs);
    const std::weak_ptr<SpecMeas> &rptr = std::get<0>(rhs);
    std::owner_less<std::weak_ptr<SpecMeas>> comp;
    const bool llr = comp(lptr,rptr);
    const bool rlr = comp(rptr,lptr);
    if( llr == rlr )
      return std::get<1>(lhs) < std::get<1>(rhs);
    return llr;
}



UndoRedoManager::UndoRedoManager( InterSpec *parent )
 : Wt::WObject( parent ),
  m_steps{},
  m_step_offset{ 0 },
  m_current_spec{},
  m_current_samples{},
  m_prev{},
  m_interspec( parent ),
  m_PeakModelChange_counter( 0 ),
  m_PeakModelChange_starting_peaks{}
{
  if( m_interspec )
  {
    m_current_spec = m_interspec->measurment( SpecUtils::SpectrumType::Foreground );
    m_current_samples = m_interspec->displayedSamples( SpecUtils::SpectrumType::Foreground );
  }//if( m_interspec )
    
  m_interspec->displayedSpectrumChanged().connect(
                boost::bind( &UndoRedoManager::handleSpectrumChange, this,
                             boost::placeholders::_1, boost::placeholders::_2,
                            boost::placeholders::_3, boost::placeholders::_4 ) );
}//UndoRedoManager


UndoRedoManager::~UndoRedoManager()
{
  
}//~UndoRedoManager()


UndoRedoManager::PeakModelChange::PeakModelChange()
{
  UndoRedoManager *manager = UndoRedoManager::instance();
  assert( manager );
  if( !manager || !manager->m_interspec )
    return;
  
  if( manager->m_PeakModelChange_counter == 0 )
  {
    auto &start_peaks = manager->m_PeakModelChange_starting_peaks;
    assert( start_peaks.empty() );
    start_peaks.clear();
    
    PeakModel *pmodel = manager->m_interspec->peakModel();
    assert( pmodel );
    if( !pmodel )
      return;
    
    shared_ptr<const deque<PeakModel::PeakShrdPtr>> peaks = pmodel->peaks();
    if( peaks )
      start_peaks.insert( end(start_peaks), begin(*peaks), end(*peaks) );
  }//if( manager->m_PeakModelChange_counter == 0 )
  
  manager->m_PeakModelChange_counter += 1;
}//PeakModelChange constructor


UndoRedoManager::PeakModelChange::~PeakModelChange()
{
  UndoRedoManager *manager = UndoRedoManager::instance();
  assert( manager );
  if( !manager || !manager->m_interspec )
    return;
  
  assert( manager->m_PeakModelChange_counter > 0 );
  if( manager->m_PeakModelChange_counter == 0 )
    return;
  
  manager->m_PeakModelChange_counter -= 1;
  if( manager->m_PeakModelChange_counter != 0 )
    return;
  
  const vector<shared_ptr<const PeakDef>> starting_peaks = manager->m_PeakModelChange_starting_peaks;
  manager->m_PeakModelChange_starting_peaks.clear();
  
  PeakModel *pmodel = manager->m_interspec->peakModel();
  assert( pmodel );
  if( !pmodel )
    return;
  
  shared_ptr<const deque<PeakModel::PeakShrdPtr>> peaks_now = pmodel->peaks();
  vector<shared_ptr<const PeakDef>> final_peaks;
  if( peaks_now )
    final_peaks.insert( end(final_peaks), begin(*peaks_now), end(*peaks_now) );
  
  if( starting_peaks == final_peaks )
    return;
  
  function<void(bool)> undo_redo = [starting_peaks,final_peaks]( const bool is_undo ){
    InterSpec *viewer = InterSpec::instance();
    assert( viewer );
    if( !viewer )
      return;
    
    PeakModel *pmodel = viewer->peakModel();
    assert( pmodel );
    if( !pmodel )
      return;
    
    pmodel->setPeaks( is_undo ? starting_peaks : final_peaks );
  };//undo_redo
  
  function<void()> undo = [undo_redo](){ undo_redo(true); };
  function<void()> redo = [undo_redo](){ undo_redo(false); };
  
  const int dpeaks = static_cast<int>(final_peaks.size()) - static_cast<int>(starting_peaks.size());
  string desc = (dpeaks == 0) ? "edit peak" : ((dpeaks < 0) ? "remove peak" : "add peak");
  if( abs(dpeaks) > 1 )
    desc += "s";
  
  manager->addUndoRedoStep( undo, redo, desc );
}//~PeakModelChange()


UndoRedoManager *UndoRedoManager::instance()
{
  InterSpec *viewer = InterSpec::instance();
  
  assert( viewer );
  if( !viewer )
    return nullptr;
  
  UndoRedoManager *manager = viewer->undoRedoManager();
  assert( manager );
  return manager;
}//UndoRedoManager::instance()


void UndoRedoManager::addUndoRedoStep( std::function<void()> undo,
                                       std::function<void()> redo,
                                       const std::string &description )
{
  assert( undo || redo );
  
  if( !m_steps )
  {
    Wt::log("debug") << "No spectrum file set for undo/redo.";
    return;
  }
  
  assert( m_step_offset <= m_steps->size() );
  
  const size_t num_steps = m_steps->size();
  
  if( (m_step_offset != 0) && (m_step_offset <= num_steps) )
  {
    // If we are here, we have hit 'undo' one or more times, and now we are making a new edit
    //  that we want an undo step for.  Instead of just discarding `m_step_offset` undo steps,
    //  like some programs, we'll instead make it so if the user hits undo, after this new step
    //  they are adding, the undo's will unwind the previous undo steps, and then re-execute
    //  the undo steps.
    for( size_t index = 0; index < m_step_offset; ++index )
    {
      UndoRedoStep step = (*m_steps)[num_steps - 1 - index];
      std::swap( step.m_redo, step.m_undo );
      m_steps->push_back( step );
    }//
  }//if( m_step_offset != 0 )
  
  m_step_offset = 0;
  m_steps->push_back( {undo, redo, description, std::chrono::system_clock::now()} );
}//void addUndoRedoStep(...)


bool UndoRedoManager::canUndo() const
{
  return m_steps && (m_step_offset < m_steps->size());
}


bool UndoRedoManager::canRedo() const
{
  return (m_step_offset > 0);
}

void UndoRedoManager::executeUndo()
{
  if( !m_steps || (m_step_offset >= m_steps->size()) )
  {
    Wt::log("debug") << "No more undo steps to execute.";
    return;
  }
  
  //UndoRedoStep &step = (*m_steps)[m_steps->size() - 1 - m_step_offset];
  UndoRedoStep *step = nullptr;
  while( m_step_offset < m_steps->size() )
  {
    UndoRedoStep *this_step = &((*m_steps)[m_steps->size() - 1 - m_step_offset]);
    m_step_offset += 1;
    
    if( this_step->m_undo )
    {
      step = this_step;
      break;
    }
  }//while( m_step_offset < m_steps->size() )
  
  assert( !step || step->m_undo );
  if( !step )
  {
    assert( m_step_offset == m_steps->size() );
    m_step_offset = m_steps->size();
    Wt::log("debug") << "No non-empty undo steps to execute.";
    return;
  }//if( !step )
  
  
  try
  {
    step->m_undo();
  }catch( std::exception &e )
  {
    const string message = "Error executing undo step: " + std::string(e.what());
    Wt::log("error") << message;
    passMessage( message, WarningWidget::WarningMsgHigh );
  }// try / catch
}//UndoRedoManager::executeUndo()


void UndoRedoManager::executeRedo()
{
  if( !m_steps || (m_step_offset == 0) || m_steps->empty() )
  {
    Wt::log("debug") << "No redo steps to execute.";
    return;
  }
  
  assert( m_step_offset <= m_steps->size() );
  if( m_step_offset > m_steps->size() )
    m_step_offset = m_steps->size();
  
  UndoRedoStep *step = nullptr;
  while( m_step_offset > 0 )
  {
    UndoRedoStep *this_step = &((*m_steps)[m_steps->size() - m_step_offset]);
    m_step_offset -= 1;
    
    if( this_step->m_redo )
    {
      step = this_step;
      break;
    }
  }//while( m_step_offset > 0 )
  
  assert( !step || step->m_redo );
  if( !step )
  {
    assert( m_step_offset == 0 );
    m_step_offset = 0;
    Wt::log("debug") << "No non-empty redo steps to execute.";
    return;
  }//if( !step )
  
  try
  {
    step->m_redo();
  }catch( std::exception &e )
  {
    const string message = "Error executing redo step: " + std::string(e.what());
    Wt::log("error") << message;
    passMessage( message, WarningWidget::WarningMsgHigh );
  }// try / catch
}//void UndoRedoManager::executeRedo()



void UndoRedoManager::handleSpectrumChange( const SpecUtils::SpectrumType type,
                                           const shared_ptr<SpecMeas> &meas,
                                           const set<int> &sample_nums,
                                           const vector<std::string> &detector_names )
{
  // TODO: perhaps we should track back/secondary spectra, so we can undo changing background/secondary
  if( type != SpecUtils::SpectrumType::Foreground )
    return;
  
  if( (meas == m_current_spec) && (sample_nums == m_current_samples) )
    return;
  
  if( m_steps && !m_steps->empty() )
  {
    std::weak_ptr<SpecMeas> prev = m_current_spec;
    const spec_key_t key{prev,m_current_samples};
    m_prev[key] = m_steps;
  }//if( !m_steps.empty() )
  
  // TODO: go through and cleanup files we are holding undo/redo for
  
  m_step_offset = 0;
  m_steps.reset();
  
  m_current_spec = meas;
  m_current_samples = sample_nums;
  
  if( meas && sample_nums.size() )
  {
    std::weak_ptr<SpecMeas> current = m_current_spec;
    const spec_key_t key{current,m_current_samples};
    const auto pos = m_prev.find( key );
    
    if( pos != end(m_prev) )
    {
      m_steps = pos->second;
      m_prev.erase( pos );
    }//if( pos != end(m_prev) )
    
    if( !m_steps )
      m_steps = make_shared<deque<UndoRedoStep>>();
  }//if( meas && sample_nums.size() )
  
  // TODO: we could insert an undo/redo step here to change back to the previous spectrum
}//void handleSpectrumChange( SpecUtils::SpectrumType type )


