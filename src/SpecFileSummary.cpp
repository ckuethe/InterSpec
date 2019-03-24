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
#include <sstream>
#include <iostream>
#include <stdexcept>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <Wt/WText>
#include <Wt/WImage>
#include <Wt/WLabel>
#include <Wt/WTable>
#include <Wt/WLineEdit>
#include <Wt/WTextArea>
#include <Wt/WComboBox>
#include <Wt/WGroupBox>
#include <Wt/WGridLayout>
#include <Wt/WPushButton>
#include <Wt/WRadioButton>
#include <Wt/WButtonGroup>
#include <Wt/WApplication>
#include <Wt/WIntValidator>
#include <Wt/WSelectionBox>
#include <Wt/WContainerWidget>

#include "InterSpec/SpecMeas.h"
#include "InterSpec/PopupDiv.h"
#include "InterSpec/AuxWindow.h"
#include "InterSpec/InterSpec.h"
#include "InterSpec/InterSpecApp.h"
#include "InterSpec/PhysicalUnits.h"
#include "InterSpec/WarningWidget.h"
#include "InterSpec/SpecFileSummary.h"
#include "SpecUtils/UtilityFunctions.h"
#include "SpecUtils/SpectrumDataStructs.h"

#if( USE_GOOGLE_MAP )
#include <Wt/WGoogleMap>
#include "InterSpec/GoogleMap.h"
#endif

using namespace std;
using namespace Wt;

namespace
{
#if( USE_GOOGLE_MAP )
  void updateCoordText( GoogleMap *googlemap,
                        double lat, double lng,
                        double origLat, double origLng )
  {
    char buffer[128];
    snprintf( buffer, sizeof(buffer), "(%.6f, %.6f)", lat, lng );
    
    googlemap->clearMeasurments();
    googlemap->addMarker( origLat, origLng );
    googlemap->addInfoBox( lat, lng, buffer );
  }//updateCoordText(...)
#endif
  
  double coordVal( Wt::WString instr )
  {
#ifndef WT_NO_STD_WSTRING
    std::wstring input = instr.value();
#else
    std::string input = instr.toUTF8();
#endif
    
    double answer = -999.9f;
    
    boost::algorithm::to_lower( input );
    boost::algorithm::replace_all(input, L"\x00B0", L" ");
    boost::algorithm::replace_all(input, L"'", L" ");
    boost::algorithm::replace_all(input, L"\"", L" ");
    boost::algorithm::replace_all(input, L"\t", L" ");
    boost::algorithm::replace_all(input, L"degree", L" ");
    boost::algorithm::replace_all(input, L"deg.", L" ");
    boost::algorithm::replace_all(input, L"deg", L" ");
    
    double sign = 1.0f;
    
    //Go through and look for n s e w to determine sign
#ifndef WT_NO_STD_WSTRING
    const wchar_t *dirs[] = { L"n", L"s", L"e", L"w" };
#else
    const char *dirs[] = { "n", "s", "e", "w" };
#endif
    
    for( size_t i = 0; i < 4; ++i )
    {
      size_t pos = input.find_first_of( dirs[i] );
      if( pos != wstring::npos )
      {
        input.erase( pos, 1 );
        sign = std::pow( -1.0, double(i) );
        break;
      }
    }//for( size_t i = 0; i < 4; ++i )
    
    //At this point, we should have nothing besides digits, spaces, decimals, and signs
    for( size_t i = 0; i < input.size(); ++i )
      if( !isdigit(input[i]) && input[i]!=' ' && input[i]!= '.' && input[i]!= '-' )
        return answer;
    
    boost::algorithm::trim( input );
    vector<std::wstring> fields;
    boost::algorithm::split( fields, input, boost::is_any_of( L" " ),
                            boost::token_compress_on );
    
    const size_t nfields = fields.size();
    
    if( nfields == 1 )
    {
      try
      {
        answer = std::stod( fields[0] );
        return answer;
      }catch(...){}
    }//if( nfields == 1 )
    
    
    if( nfields==3 )
    {
      try
      {
        const float deg = static_cast<float>( std::stod( fields[0] ) );
        const float min = static_cast<float>( std::stod( fields[1] ) );
        const float sec = static_cast<float>( std::stod( fields[2] ) );
        answer = deg + (min/60.0f) + (sec/3600.0f);
        return sign * answer;
      }catch(...){}
    }//if( nfields==3 )
    
    //If were here, weve failed
    return answer;
  }//float coordVal( std::wstring input )
}//namespace


//Class to display a DetectorAnalysis; just a first go at it
//  Some sort of model and table, or something might be a better implem
class AnaResultDisplay : public WContainerWidget
{
  
  
  WText *m_summary;
  std::shared_ptr<const SpecMeas> m_meas;
  
  WTable *m_table;
  bool m_modifiable;
  
  enum AnaResultEditableFields
  {
    AlgorithmName,
    AlgorithmVersion,
    AlgorithmCreator,
    AlgorithmDescription,
    AlgorithmResultDescription,
    AlgortihmRemarks
  };//enum AnaResultEditableFields
  
  WLineEdit *m_algorithm_name;
  WLineEdit *m_algorithm_version;
  WLineEdit *m_algorithm_creator;
  WLineEdit *m_algorithm_description;
  WLineEdit *m_algorithm_result_description;
  WTextArea *m_algorithm_remarks;
  
  void handleFieldUpdate( AnaResultEditableFields field )
  {
    switch( field )
    {
      case AlgorithmName:
      case AlgorithmVersion:
      case AlgorithmCreator:
      case AlgorithmDescription:
      case AlgorithmResultDescription:
      case AlgortihmRemarks:
        break;
    }//switch( field )
    
    passMessage( "Editing Analysis Results Not implemented - sorry", "", WarningWidget::WarningMsgInfo );
  }//void handleFieldUpdate( AnaResultEditableFields field )
  
  template <class T>
  void addField( T *&edit, WTable *table, const WString &labelstr,
                int row, int col, int rowspan = 1, int colspan = 1 )
  {
    WLabel *label = new WLabel(labelstr);
    label->setStyleClass("SpectrumFileSummaryLabel");
    WTableCell *cell = table->elementAt(row, col);
    cell->setRowSpan( rowspan );
    cell->addWidget( label );
    edit = new T();
    cell = table->elementAt(row, col+1);
    cell->setRowSpan( rowspan );
    cell->setColumnSpan( colspan );
    cell->addWidget( edit );
    label->setBuddy( edit );
    edit->disable();
  }//WLineEdit *addField(...)

  void addField( WText *&edit, WTable *table, const WString &labelstr,
                int row, int col, int rowspan = 1, int colspan = 1 )
  {
    WLabel *label = new WLabel(labelstr);
    label->setStyleClass("SpectrumFileSummaryLabel");
    WTableCell *cell = table->elementAt(row, col);
    cell->setRowSpan( rowspan );
    cell->addWidget( label );
    
    cell = table->elementAt(row, col+1);
    cell->setRowSpan( rowspan );
    cell->setColumnSpan( colspan );
    edit = new WText();
    cell->addWidget( edit );
    edit->disable();
  }
  
public:
  AnaResultDisplay( WContainerWidget *parent = 0 )
  : WContainerWidget( parent ), m_summary( NULL ), m_table( NULL ),
    m_modifiable( false )
  {
    m_table = new WTable( this );
    
    addField( m_algorithm_name, m_table, "Algo Name", AlgorithmName, 0 );
    addField( m_algorithm_version, m_table, "Algo Version", AlgorithmVersion, 0 );
    addField( m_algorithm_creator, m_table, "Algo Creator", AlgorithmCreator, 0 );
    addField( m_algorithm_description, m_table, "Algo Desc.", AlgorithmDescription, 0 );
    addField( m_algorithm_result_description, m_table, "Result Desc", AlgorithmResultDescription, 0 );
    addField( m_algorithm_remarks, m_table, "Remarks", AlgortihmRemarks, 0 );
    
    m_algorithm_name->changed().connect( boost::bind( &AnaResultDisplay::handleFieldUpdate, this, AlgorithmName) );
    m_algorithm_version->changed().connect( boost::bind( &AnaResultDisplay::handleFieldUpdate, this,AlgorithmVersion ) );
    m_algorithm_creator->changed().connect( boost::bind( &AnaResultDisplay::handleFieldUpdate, this, AlgorithmCreator ) );
    m_algorithm_description->changed().connect( boost::bind( &AnaResultDisplay::handleFieldUpdate, this, AlgorithmDescription ) );
    m_algorithm_result_description->changed().connect( boost::bind( &AnaResultDisplay::handleFieldUpdate, this, AlgorithmResultDescription ) );
    m_algorithm_remarks->changed().connect( boost::bind( &AnaResultDisplay::handleFieldUpdate, this, AlgortihmRemarks ) );
  }//constructor
  
  void updateDisplay( std::shared_ptr<const SpecMeas> meas )
  {
    m_meas = meas;
    std::shared_ptr<const DetectorAnalysis> ana;
    if( meas )
      ana = meas->detectors_analysis();
    
    if( m_summary )
      delete m_summary;
    m_summary = NULL;
    
    if( !ana )
    {
      m_algorithm_name->setText( "" );
      m_algorithm_version->setText( "" );
      m_algorithm_creator->setText( "" );
      m_algorithm_description->setText( "" );
      m_algorithm_result_description->setText( "" );
      m_algorithm_remarks->setText( "" );
      return;
    }
  
    m_algorithm_name->setText( ana->algorithm_name_ );
    m_table->rowAt(AlgorithmName)->setHidden( m_algorithm_name->text().empty() );
    
    m_algorithm_version->setText( ana->algorithm_version_ );
    m_table->rowAt(AlgorithmVersion)->setHidden( m_algorithm_version->text().empty() );
    
    m_algorithm_creator->setText( ana->algorithm_creator_ );
    m_table->rowAt(AlgorithmCreator)->setHidden( m_algorithm_creator->text().empty() );
    
    m_algorithm_description->setText( ana->algorithm_description_ );
    m_table->rowAt(AlgorithmDescription)->setHidden( m_algorithm_description->text().empty() );
    
    m_algorithm_result_description->setText( ana->algorithm_result_description_ );
    m_table->rowAt(AlgorithmResultDescription)->setHidden( m_algorithm_result_description->text().empty() );
    
    string remarktxt;
    for( const string &r : ana->remarks_ )
      remarktxt += (remarktxt.size() ? "\n" : "") + r;
    
    m_algorithm_remarks->setText( remarktxt );
    m_table->rowAt(AlgortihmRemarks)->setHidden( m_algorithm_remarks->text().empty() );
    
    //Now Display the Nuclide results - hacking for now - should make dedicated
    //  widget
    string anastr;
    
    for( const DetectorAnalysisResult &res : ana->results_ )
    {
      string result;
      if( res.nuclide_.size() )
        result = "<tr><td>Identified</td><td>" + res.nuclide_ + "</td></tr>";
      if( res.nuclide_type_.size() )
        result += "<tr><td>Category</td><td>" + res.nuclide_type_ + "</td></tr>";
      if( res.id_confidence_.size() )
        result += "<tr><td>Confidence</td><td>" + res.id_confidence_ + "</td></tr>";
      if( res.detector_.size() )
        result += "<tr><td>Detector</td><td>" + res.detector_ + "</td></tr>";
      
      if( res.dose_rate_ > 0.0 )
        result += "<tr><td>Dose</td><td>"
                  + PhysicalUnits::printToBestDoseUnits( 1.0E-6 * res.dose_rate_*PhysicalUnits::sievert/PhysicalUnits::hour ) + "</td></tr>";
      if( res.distance_ > 0.0 )
        result += "<tr><td>Distance</td><td>" + PhysicalUnits::printToBestLengthUnits(0.1*res.distance_) + "</td></tr>";
      if( res.activity_ > 0.0 )
        result += "<tr><td>Activity</td><td>" + PhysicalUnits::printToBestActivityUnits( res.activity_, 2, true, 1.0 ) + "</td></tr>";
      if( res.remark_.size() > 0 )
        result += "<tr><td>Remark</td><td>" + res.remark_ + "</td></tr>";
      
      //  float real_time_;           //in units of seconds (eg: 1.0 = 1 s)
      //  boost::posix_time::ptime start_time_;
      
      if( !result.empty() )
        anastr += "<table>" + result + "</table>";
    }
    
    if( anastr.size() > 2 )
    {
      m_summary = new WText( anastr );
      m_summary->setInline( false );
      this->addWidget( m_summary );
    }
  }//void updateDisplay( std::shared_ptr<const SpecMeas> meas )
  
  void allowModifiable( bool allow )
  {
    //unimplemented
    m_modifiable = allow;
    
    m_algorithm_name->setEnabled( allow );
    m_algorithm_version->setEnabled( allow );
    m_algorithm_creator->setEnabled( allow );
    m_algorithm_description->setEnabled( allow );
    m_algorithm_result_description->setEnabled( allow );
    m_algorithm_remarks->setEnabled( allow );
    
    m_table->rowAt(AlgorithmName)->setHidden( !allow && m_algorithm_name->text().empty() );
    m_table->rowAt(AlgorithmVersion)->setHidden( !allow && m_algorithm_version->text().empty() );
    m_table->rowAt(AlgorithmCreator)->setHidden( !allow && m_algorithm_creator->text().empty() );
    m_table->rowAt(AlgorithmDescription)->setHidden( !allow && m_algorithm_description->text().empty() );
    m_table->rowAt(AlgorithmResultDescription)->setHidden( !allow && m_algorithm_result_description->text().empty() );
  }//void allowModifiable( bool allow )
  
};//class AnaResultDisplay

SpecFileSummary::SpecFileSummary( InterSpec *specViewer )
  : AuxWindow( "File Parameters" ),
    m_specViewer( specViewer ),
    m_allowEditGroup( NULL ),
    m_displaySampleDiv( NULL ),
    m_displaySampleNumEdit( NULL ),
    m_displayedPreText( NULL ),
    m_displayedPostText( NULL ),
    m_nextSampleNumButton( NULL ),
    m_prevSampleNumButton( NULL ),
    m_displaySampleNumValidator( NULL ),
    m_gammaCPS( NULL ),
    m_gammaSum( NULL ),
    m_neutronCPS( NULL ),
    m_neutronSum( NULL ),
    m_displayedLiveTime( NULL ),
    m_displayedRealTime( NULL ),
    m_timeStamp( NULL ),
    m_energyRange( NULL ),
    m_numberOfBins( NULL ),
    m_detector( NULL ),
    m_sampleNumber( NULL ),
    m_measurmentRemarks( NULL ),
    m_longitude( NULL ),
    m_latitude( NULL ),
    m_gpsTimeStamp( NULL ),
#if( USE_GOOGLE_MAP )
    m_showMapButton( NULL ),
#endif
    m_title( NULL ),
    m_source( NULL ),
    m_spectraGroup( NULL ),
    m_fileRemarks( NULL ),
    m_sizeInMemmory( NULL ),
    m_filename( NULL ),
    m_uuid( NULL ),
    m_laneNumber( NULL ),
    m_measurement_location_name( NULL ),
    m_ana_button( NULL ),
    m_ana_results( NULL ),
    m_inspection( NULL ),
    m_instrument_type( NULL ),
    m_manufacturer( NULL ),
    m_instrument_model( NULL ),
    m_instrument_id( NULL ),
    m_reloadSpectrum( NULL )
{
  init();
}//SpecFileSummary constructor


template <class T>
void addField( T *&edit, WGridLayout *table, const WString &labelstr,
               int row, int col, int rowspan = 1, int colspan = 1 )
{
  WLabel *label = new WLabel(labelstr);
  label->setStyleClass("SpectrumFileSummaryLabel");
  table->addWidget( label, row, col, AlignMiddle );
  edit = new T();
  table->addWidget( edit, row, col+1, rowspan, colspan );
  label->setBuddy( edit );
}//WLineEdit *addField(...)


void addField( WText *&edit, WGridLayout *table, const WString &labelstr,
              int row, int col, int rowspan = 1, int colspan = 1 )
{
  WLabel *label = new WLabel(labelstr);
  label->setStyleClass("SpectrumFileSummaryLabel");
  table->addWidget( label, row, col, (rowspan==1 ? AlignMiddle : AlignBottom) );
  edit = new WText();
  table->addWidget( edit, row, col+1, rowspan, colspan, AlignMiddle | AlignLeft );
}



void SpecFileSummary::init()
{
  wApp->useStyleSheet( "InterSpec_resources/SpecFileSummary.css" );
  
  WContainerWidget *holder = new WContainerWidget( contents() );
  
  WGridLayout *overallLayout = new WGridLayout();
  holder->addStyleClass( "SpecFileSummary" );
  holder->setLayout( overallLayout );
  
  overallLayout->setContentsMargins( 9, 0, 9, 0 );
  
  WRadioButton *button = NULL;
  WGroupBox *spectrumGroupBox = new WGroupBox( "Spectrum" );
  spectrumGroupBox->addStyleClass( "SpecSummChoose" );
  m_spectraGroup = new WButtonGroup( this );
  button = new WRadioButton( "Foreground", spectrumGroupBox );
  m_spectraGroup->addButton( button, kForeground );
  button = new WRadioButton( "Secondary", spectrumGroupBox );
  m_spectraGroup->addButton( button, kSecondForeground );
  button = new WRadioButton( "Background", spectrumGroupBox );
  m_spectraGroup->addButton( button, kBackground );
  m_spectraGroup->setCheckedButton( m_spectraGroup->button(kForeground) );
  m_spectraGroup->checkedChanged().connect( this, &SpecFileSummary::handleSpectrumTypeChanged );
  
  
  WGroupBox *editGroupBox = new WGroupBox( "Allow Edit" );
  editGroupBox->addStyleClass( "SpecSummAllowEdit" );
  m_allowEditGroup = new WButtonGroup( this );
  button = new WRadioButton( "Yes", editGroupBox );
  m_allowEditGroup->addButton( button, kAllowModify );
  button = new WRadioButton( "No", editGroupBox );
  m_allowEditGroup->addButton( button, kDontAllowModify );
  m_allowEditGroup->setCheckedButton( m_allowEditGroup->button(kDontAllowModify) );
  m_allowEditGroup->checkedChanged().connect( this, &SpecFileSummary::handleAllowModifyStatusChange );
  
  WContainerWidget *upperdiv = new WContainerWidget();
  WGridLayout *upperlayout = new WGridLayout();
  upperdiv->setLayout( upperlayout );
  overallLayout->addWidget( upperdiv, 0, 0 );
  upperlayout->addWidget( spectrumGroupBox, 0, 0, 1, 1 );
  upperlayout->addWidget( editGroupBox, 0, 1, 1, 1 );
  upperlayout->setColumnStretch( 1, 1 );
  
  m_reloadSpectrum = new WPushButton( "Update Displays", editGroupBox );
  m_reloadSpectrum->setStyleClass("RefreshIcon");
  m_reloadSpectrum->setToolTip( "The changes made on this screen may not be"
                               " propogated to othe GUI compnents untill you"
                               " reload the spectrum." );
  m_reloadSpectrum->setFloatSide( Wt::Right );
  m_reloadSpectrum->clicked().connect( this, &SpecFileSummary::reloadCurrentSpectrum );
  m_reloadSpectrum->disable();
  

  WGroupBox *filediv = new WGroupBox( "File Information" );
  filediv->addStyleClass( "SpecFileInfo" );
  WGridLayout *fileInfoLayout = new WGridLayout();
  filediv->setLayout( fileInfoLayout );
  overallLayout->addWidget( filediv, 1, 0 );
  
  addField( m_filename, fileInfoLayout, "File Name:", 0, 0, 1, 5 );
  addField( m_sizeInMemmory, fileInfoLayout, "Mem Size:", 0, 6 );
  addField( m_inspection, fileInfoLayout, "Inspection:", 1, 0 );
  addField( m_laneNumber, fileInfoLayout, "Lane:", 1, 2 );
  addField( m_measurement_location_name, fileInfoLayout, "Location:", 1, 4, 1, 1 );
  
  m_ana_button = new WPushButton( "RIID Analysis" );
  fileInfoLayout->addWidget( m_ana_button, 1, 6, 1, 2, AlignLeft );
  PopupDivMenu *popup = new PopupDivMenu( m_ana_button, PopupDivMenu::TransientMenu );
  
  m_ana_results = new AnaResultDisplay();
  popup->addWidget( m_ana_results );
  
  addField( m_instrument_type, fileInfoLayout, "Instru. Type:", 2, 0 );
  addField( m_manufacturer, fileInfoLayout, "Manufacturer:", 2, 2 );
  addField( m_instrument_model, fileInfoLayout, "Model:", 2, 4, 1, 3 );
  
  addField( m_instrument_id, fileInfoLayout, "Instru. ID:", 3, 0, 1, 3 );
  addField( m_uuid, fileInfoLayout, "UUID:", 3, 4, 1, 3 );
  
  WContainerWidget *labelHolder = new WContainerWidget();
  labelHolder->setAttributeValue( "style", "margin-top: auto; margin-bottom: auto;" );
  WLabel *label = new WLabel( "File", labelHolder );
  label->setStyleClass("SpectrumFileSummaryLabel");
  label->setInline( false );
  label = new WLabel( "Remarks:", labelHolder );
  label->setStyleClass("SpectrumFileSummaryLabel");
  label->setInline( false );
  fileInfoLayout->addWidget( labelHolder, 4, 0, AlignMiddle );
  
  WContainerWidget *remarkHolder = new WContainerWidget();
  fileInfoLayout->addWidget( remarkHolder, 4, 1, 1, 7 );
  m_fileRemarks = new WTextArea( remarkHolder );
  m_fileRemarks->setAttributeValue( "style", "resize: none; width: 100%; height: 100%;" );
  
  fileInfoLayout->setRowStretch( 4, 1 );
  
  fileInfoLayout->setColumnStretch( 1, 1 );
  fileInfoLayout->setColumnStretch( 3, 1 );
  fileInfoLayout->setColumnStretch( 5, 1 );
  fileInfoLayout->setColumnStretch( 7, 1 );

  
  WGroupBox *measdiv = new WGroupBox( "Measurment Information" );
  measdiv->addStyleClass( "MeasInfo" );
  WGridLayout *measTable = new WGridLayout();
  measdiv->setLayout( measTable );
  overallLayout->addWidget( measdiv, 2, 0 );
  
  
  addField( m_timeStamp, measTable, "Date/Time:", 0, 0, 1, 3 );
  addField( m_displayedLiveTime, measTable, "Live Time:", 0, 4 );
  addField( m_displayedRealTime, measTable, "Real Time:", 0, 6 );
  addField( m_detector, measTable, "Det. Name:", 1, 0 );
  addField( m_sampleNumber, measTable, "Sample Num:", 1, 2 );
  addField( m_energyRange, measTable, "Energy(keV):", 1, 4 );
  addField( m_numberOfBins, measTable, "Num Channels:", 1, 6 );
  addField( m_gammaSum, measTable, "Sum Gamma:", 2, 0 );
  addField( m_gammaCPS, measTable, "Gamma CPS:", 2, 2 );
  addField( m_neutronSum, measTable, "Sum Neutron:", 2, 4 );
  addField( m_neutronCPS, measTable, "Neutron CPS:", 2, 6 );
  addField( m_latitude, measTable, "Latitude:", 3, 0 );
  addField( m_longitude, measTable, "Longitude:", 3, 2 );
  addField( m_gpsTimeStamp, measTable, "Position Time:", 3, 4 );
  
  m_latitude->setEmptyText( "dec or deg min' sec\" N/S" );
  m_longitude->setEmptyText( "dec or deg min' sec\" E/W" );
  
#if( USE_GOOGLE_MAP )
  m_showMapButton = new WPushButton( "Show Map" );
  measTable->addWidget( m_showMapButton, 3, 6 );
  m_showMapButton->clicked().connect( this, &SpecFileSummary::showGoogleMap );
  m_showMapButton->disable();
#endif
  
  addField( m_title, measTable, "Description:", 4, 0, 1, 5 );
  
  addField( m_source, measTable, "Source Type:", 4, 6 );
  
  for( int i = 0; i <= Measurement::UnknownSourceType; ++i )
  {
    const char *val = "";
    switch( i )
    {
      case Measurement::IntrinsicActivity: val = "Intrinsic Activity"; break;
      case Measurement::Calibration:       val = "Calibration";        break;
      case Measurement::Background:        val = "Background";         break;
      case Measurement::Foreground:        val = "Foreground";         break;
      case Measurement::UnknownSourceType: val = "Unknown";            break;
      default: break;
    }//switch( i )
    
    m_source->addItem( val );
  }//for( int i = 0; i <= Measurement::UnknownSourceType; ++i )
  

  labelHolder = new WContainerWidget();
  labelHolder->setAttributeValue( "style", "margin-top: auto; margin-bottom: auto;" );
  label = new WLabel( "Spectra", labelHolder );
  label->setStyleClass("SpectrumFileSummaryLabel");
  label->setInline( false );
  label = new WLabel( "Remarks:", labelHolder );
  label->setStyleClass("SpectrumFileSummaryLabel");
  label->setInline( false );
  measTable->addWidget( labelHolder, 5, 0, AlignMiddle );
  
  remarkHolder = new WContainerWidget();
  measTable->addWidget( remarkHolder, 5, 1, 1, 7 );
  m_measurmentRemarks = new WTextArea( remarkHolder );
  m_measurmentRemarks->setAttributeValue( "style", "resize: none; width: 100%; height: 100%;" );
  
  measTable->setRowStretch( 5, 1 );
  
  measTable->setColumnStretch( 1, 1 );
  measTable->setColumnStretch( 3, 1 );
  measTable->setColumnStretch( 5, 1 );
  measTable->setColumnStretch( 7, 1 );
  
  
  m_displaySampleDiv = new WContainerWidget();
  m_displaySampleDiv->addStyleClass( "displaySampleDiv" );
  m_prevSampleNumButton = new WImage( "InterSpec_resources/images/previous_arrow.png", m_displaySampleDiv );
  m_prevSampleNumButton->clicked().connect( boost::bind( &SpecFileSummary::handleUserIncrementSampleNum, this, false) );
  m_displayedPreText = new WText( m_displaySampleDiv );
  m_displaySampleNumEdit = new WLineEdit( m_displaySampleDiv );
  m_displaySampleNumValidator = new WIntValidator( m_displaySampleDiv );
  m_displaySampleNumEdit->setValidator( m_displaySampleNumValidator );
  m_displaySampleNumEdit->addStyleClass( "numberValidator"); //used to detect mobile keyboard
  m_displaySampleNumEdit->setTextSize( 3 );
  m_displayedPostText = new WText( m_displaySampleDiv );
  m_nextSampleNumButton = new WImage( "InterSpec_resources/images/next_arrow.png", m_displaySampleDiv );
  m_nextSampleNumButton->clicked().connect( boost::bind( &SpecFileSummary::handleUserIncrementSampleNum, this, true) );
  m_displaySampleNumEdit->enterPressed().connect( boost::bind( &SpecFileSummary::handleUserChangeSampleNum, this ) );
  m_displaySampleNumEdit->blurred().connect( boost::bind( &SpecFileSummary::handleUserChangeSampleNum, this ) );
  m_displaySampleDiv->setHiddenKeepsGeometry( false );
  
  measTable->addWidget( m_displaySampleDiv, measTable->rowCount(), 0, 1, measTable->columnCount(), AlignBottom );
  
  overallLayout->setRowStretch( 1, 1 );
  overallLayout->setRowStretch( 2, 1 );

  m_specViewer->displayedSpectrumChanged().connect( boost::bind( &SpecFileSummary::handleSpectrumChange, this, _1, _2, _3 ) );


  m_displayedLiveTime->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDisplayedLiveTime) );
  m_displayedLiveTime->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDisplayedLiveTime) );
  m_displayedLiveTime->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDisplayedLiveTime) );

  m_displayedRealTime->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDisplayedRealTime) );
  m_displayedRealTime->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDisplayedRealTime) );
  m_displayedRealTime->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDisplayedRealTime) );

  m_timeStamp->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kTimeStamp) );
  m_timeStamp->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kTimeStamp) );
  m_timeStamp->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kTimeStamp) );

  m_longitude->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  m_longitude->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  m_longitude->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );

  m_latitude->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  m_latitude->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  m_latitude->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );

  m_gpsTimeStamp->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  m_gpsTimeStamp->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  m_gpsTimeStamp->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kPosition) );
  
  m_title->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDescription) );
  m_title->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDescription) );
  m_title->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kDescription) );
  
  m_source->activated().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kSourceType) );
  
  m_measurmentRemarks->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kMeasurmentRemarks) );
  m_measurmentRemarks->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kMeasurmentRemarks) );
  m_measurmentRemarks->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kMeasurmentRemarks) );

  m_fileRemarks->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kFileRemarks) );
  m_fileRemarks->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kFileRemarks) );
  m_fileRemarks->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kFileRemarks) );

  m_filename->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kFilename) );
  m_filename->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kFilename) );
  m_filename->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kFilename) );

  m_uuid->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kUuid) );
  m_uuid->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kUuid) );
  m_uuid->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kUuid) );

  m_laneNumber->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kLaneNumber) );
  m_laneNumber->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kLaneNumber) );
  m_laneNumber->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kLaneNumber) );

  m_measurement_location_name->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kMeasurement_location_name) );
  m_measurement_location_name->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kMeasurement_location_name) );
  m_measurement_location_name->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kMeasurement_location_name) );

  m_inspection->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInspection) );
  m_inspection->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInspection) );
  m_inspection->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInspection) );

  m_instrument_type->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_type) );
  m_instrument_type->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_type) );
  m_instrument_type->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_type) );

  m_manufacturer->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kManufacturer) );
  m_manufacturer->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kManufacturer) );
  m_manufacturer->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kManufacturer) );

  m_instrument_model->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_model) );
  m_instrument_model->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_model) );
  m_instrument_model->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_model) );

  m_instrument_id->changed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_id) );
  m_instrument_id->enterPressed().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_id) );
  m_instrument_id->blurred().connect( boost::bind( &SpecFileSummary::handleFieldUpdate, this, kInstrument_id) );


  handleSpectrumTypeChanged();
  handleAllowModifyStatusChange();
  finished().connect( boost::bind( &AuxWindow::deleteAuxWindow, this ) );
  
  AuxWindow::addHelpInFooter( footer(), "file-parameters-dialog" );

  WPushButton *closeButton = addCloseButtonToFooter();
  closeButton->clicked().connect( boost::bind( &AuxWindow::deleteAuxWindow, this ) );
  
  holder->setMinimumSize( 780, 500 );
  holder->setWidth( WLength(100.0,WLength::Percentage) );
  holder->setHeight( WLength(100.0,WLength::Percentage) );
  WDialog::contents()->setOverflow( WContainerWidget::OverflowAuto );
  
//  setMinimumSize( 780, 500 );
  resizeScaledWindow(0.85,0.85);
  refresh();
  
  centerWindow();
  rejectWhenEscapePressed();
  setResizable( true );
  show();
}//void SpecFileSummary::init()


SpecFileSummary::~SpecFileSummary()
{
  //nothing to do here
}


#if( USE_GOOGLE_MAP )
void SpecFileSummary::showGoogleMap()
{
  if( !m_specViewer )
    return;
  
  double longitude = coordVal( m_longitude->text() );
  double latitude = coordVal( m_latitude->text() );
  if( fabs(longitude) > 180.0 )
    longitude = -999.9;
  if( fabs(latitude) > 90.0 )
    latitude = -999.9;
  
  if( fabs(latitude)>999.0 || fabs(longitude)>999.0 )
    return;
 

  AuxWindow *window = new AuxWindow( "Google Map" );
  
  const int w = static_cast<int>(0.66*m_specViewer->renderedWidth());
  const int h = static_cast<int>(0.8*m_specViewer->renderedHeight());
  
 
  window->disableCollapse();
  window->finished().connect( boost::bind( &AuxWindow::deleteAuxWindow, window ) );
 // window->footer()->setStyleClass( "modal-footer" );
  window->footer()->setHeight(WLength(50,WLength::Pixel));
  WPushButton *closeButton = window->addCloseButtonToFooter();
  closeButton->clicked().connect( window, &AuxWindow::hide );
  

  WGridLayout *layout = window->stretcher();
  GoogleMap *googlemap = new GoogleMap( false );
  googlemap->addMarker( latitude, longitude );
  googlemap->adjustPanAndZoom();
  googlemap->mapClicked().connect( boost::bind( &updateCoordText, googlemap, _1, _2, latitude, longitude ) );
  layout->addWidget( googlemap, 0, 0 );
  window->resize( WLength(w), WLength(h) );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setVerticalSpacing( 0 );
  layout->setHorizontalSpacing( 0 );

  window->show();
  window->centerWindow();
  window->rejectWhenEscapePressed();
  //  window->resizeToFitOnScreen();
  
//  char buffer[128];
//  snprintf( buffer, sizeof(buffer), "(%.6f, %.6f)", latitude, longitude );
//  coordTxt->setText( buffer );
}//void showGoogleMap();
#endif //#if( USE_GOOGLE_MAP )


void SpecFileSummary::updateMeasurmentFieldsFromMemory()
{
  char buffer[64];
  try
  {
    std::shared_ptr<const Measurement> sample = currentMeasurment();

    if( !sample )
      throw runtime_error("");

    const float liveTime = float(sample->live_time() / PhysicalUnits::second);
    const float realTime = float(sample->real_time() / PhysicalUnits::second);

    const double sumGamma = sample->gamma_count_sum();
    const double gammaCps = (liveTime > FLT_EPSILON) ? sumGamma/liveTime : -1.0;
    
    snprintf( buffer, sizeof(buffer), "%.3f", gammaCps );
    if( gammaCps < -FLT_EPSILON )
      m_gammaCPS->setText( "--" );
    else
      m_gammaCPS->setText( buffer );
    
    snprintf( buffer, sizeof(buffer), "%.2f", sumGamma );
    m_gammaSum->setText( buffer );

    if( sample->contained_neutron() )
    {
      const double sumNeutron = sample->neutron_counts_sum();
      const double neutronCps = (realTime > FLT_EPSILON) ? sumNeutron/realTime : -1.0;

      snprintf( buffer, sizeof(buffer), "%.3f", neutronCps );
      if( neutronCps < -FLT_EPSILON )
        m_neutronCPS->setText( "--" );
      else
        m_neutronCPS->setText( buffer );
      
      snprintf( buffer, sizeof(buffer), "%.2f", sumNeutron );
      m_neutronSum->setText( buffer );
    }else
    {
      m_neutronCPS->setText( "N/A" );
      m_neutronSum->setText( "N/A" );
    }

    snprintf( buffer, sizeof(buffer), "%.2f s", liveTime );
    m_displayedLiveTime->setText( buffer );
    snprintf( buffer, sizeof(buffer), "%.2f s", realTime );
    m_displayedRealTime->setText( buffer );

    if( fabs( sample->latitude() ) < 999.0 )
    {
      snprintf( buffer, sizeof(buffer), "%.6f", sample->latitude() );
      m_latitude->setText( buffer );
    }else
      m_latitude->setText( "" );
    if( fabs(sample->longitude()) < 999.0 )
    {
      snprintf( buffer, sizeof(buffer), "%.6f", sample->longitude() );
      m_longitude->setText( buffer );
    }else
      m_longitude->setText( "" );

#if( USE_GOOGLE_MAP )
    bool nomap = (fabs(sample->latitude())>999.0 || fabs(sample->longitude())>999.0);
    m_showMapButton->setDisabled( nomap );
#endif
    
    if( sample->position_time().is_special() )
    {
      m_gpsTimeStamp->setText( "" );
    }else
    {
      stringstream gpsTimeStampStrm;
      gpsTimeStampStrm << sample->position_time();
      m_gpsTimeStamp->setText( gpsTimeStampStrm.str() );
    }


    stringstream timeStampStrm;
    timeStampStrm << sample->start_time();
    m_timeStamp->setText( timeStampStrm.str() );


    ShrdConstFVecPtr binning = sample->channel_energies();

    if( binning && binning->size() )
    {
      const size_t nbin = binning->size();
      float lowEnergy = binning->at(0);
      float upperEnergy = binning->at(nbin-1);

      if( nbin > 2 )
        upperEnergy += (binning->at(nbin-1) - binning->at(nbin-2));

      snprintf( buffer, sizeof(buffer), "%.1f to %.1f", lowEnergy, upperEnergy );
      m_energyRange->setText( buffer );
      m_numberOfBins->setText( std::to_string(nbin) );
    }else
    {
      m_energyRange->setText( "" );
      m_numberOfBins->setText( "" );
    }//if( binning ) / else

    m_detector->setText( sample->detector_name() );

    const int specNum = sample->sample_number();
    m_sampleNumber->setText( std::to_string(specNum) );

    string remark;
    const vector<string> &remarks = sample->remarks();
    for( size_t i = 0; i < remarks.size(); ++i )
    {
      if( i )
        remark += '\n';
      remark += remarks[i];
    }//for( size_t i = 0; i < remarks.size(); ++i )

    m_title->setText( WString::fromUTF8(sample->title()) );
    m_source->setCurrentIndex( sample->source_type() );
    
    m_measurmentRemarks->setText( remark );
  }catch(...)
  {
    m_gammaCPS->setText( "-" );
    m_gammaSum->setText( "-" );
    m_neutronCPS->setText( "-" );
    
    m_longitude->setText( "" );
    m_latitude->setText( "" );
    m_gpsTimeStamp->setText( "" );
#if( USE_GOOGLE_MAP )
    m_showMapButton->disable();
#endif
    
    m_neutronSum->setText( "-" );
    m_displayedLiveTime->setText( "" );
    m_displayedRealTime->setText( "" );
    m_timeStamp->setText( "" );

    m_title->setText( "" );
    m_source->setCurrentIndex( -1 );

    m_energyRange->setText( "-" );
    m_numberOfBins->setText( "-" );

    m_detector->setText( "-" );
    m_sampleNumber->setText( "-" );
    m_measurmentRemarks->setText( "-" );
  }//try / catch
}//void updateMeasurmentFieldsFromMemory()


void SpecFileSummary::updateDisplayFromMemory()
{
//  return;
  const SpectrumType type = SpectrumType(m_spectraGroup->checkedId());

  std::shared_ptr<const SpecMeas> meas = m_specViewer->measurment( type );
  const size_t nspec = (!!meas ? meas->measurements().size() : 0);

  m_displaySampleDiv->setHidden( nspec < 2 );
  const bool hasForeground = !!m_specViewer->measurment(kForeground);
  const bool hasSecondFore = !!m_specViewer->measurment(kSecondForeground);
  const bool hasBackground = !!m_specViewer->measurment(kBackground);
  m_spectraGroup->button(kForeground)->setEnabled( hasForeground );
  m_spectraGroup->button(kSecondForeground)->setEnabled( hasSecondFore );
  m_spectraGroup->button(kBackground)->setEnabled( hasBackground );
  
  
  m_displayedPostText->setText( "of " + std::to_string(nspec) );
  if( nspec )
  {
    m_displaySampleNumValidator->setRange( 1, static_cast<int>(nspec) );
    m_displaySampleNumEdit->setText( "1" );
  }else
  {
    m_displaySampleNumValidator->setRange( 0, 0 );
    m_displaySampleNumEdit->setText( "" );
  }//if( nspec ) / else

  if( !!meas )
  {
    double memsize = static_cast<double>( meas->memmorysize() );
    const char *memunit = "b";

    if( memsize > 1024*1024 )
    {
      memunit = "Mb";
      memsize /= (1024.0*1024.0);
    }else if( memsize > 1024 )
    {
      memunit = "kb";
      memsize /= 1024.0;
    }//

    char buffer[32];
    snprintf( buffer, sizeof(buffer), "%.1f %s", memsize, memunit );
    m_sizeInMemmory->setText( buffer );
    m_filename->setText( meas->filename() );
    m_uuid->setText( meas->uuid() );

    try
    {
      if( meas->lane_number() < 0 )
        throw std::runtime_error( "" );
      m_laneNumber->setText( std::to_string( meas->lane_number() ) );
    }catch(...)
    {
      m_laneNumber->setText( "" );
    }

    m_measurement_location_name->setText( meas->measurement_location_name() );
    
    m_ana_results->updateDisplay( meas );
    
    m_inspection->setText( meas->inspection() );
    m_instrument_type->setText( meas->instrument_type() );
    m_manufacturer->setText( meas->manufacturer() );
    m_instrument_model->setText( meas->instrument_model() );
    m_instrument_id->setText( meas->instrument_id() );
    
    string remark;
    const vector<string> &remarks = meas->remarks();
    for( size_t i = 0; i < remarks.size(); ++i )
    {
      if( i )
        remark += '\n';
      remark += remarks[i];
    }//for( size_t i = 0; i < remarks.size(); ++i )

    m_fileRemarks->setText( WString::fromUTF8(remark) );
  }else
  {
    m_sizeInMemmory->setText( "" );
    m_filename->setText( "" );
    m_uuid->setText( "" );
    m_laneNumber->setText( "" );
    m_measurement_location_name->setText( "" );
    m_ana_results->updateDisplay( std::shared_ptr<const SpecMeas>() );
    m_inspection->setText( "" );
    m_instrument_type->setText( "" );
    m_manufacturer->setText( "" );
    m_instrument_model->setText( "" );
    m_instrument_id->setText( "" );
    m_fileRemarks->setText( "" );
  }//if( meas ) / else
  
  updateMeasurmentFieldsFromMemory();
}//void SpecFileSummary::updateDisplayFromMemory()

std::shared_ptr<const Measurement> SpecFileSummary::currentMeasurment() const
{
  MeasurementConstShrdPtr sample;

  try
  {
    const SpectrumType type = SpectrumType(m_spectraGroup->checkedId());
    std::shared_ptr<const SpecMeas> meas = m_specViewer->measurment( type );
    if( !meas )
      throw runtime_error( "" );

    const vector<MeasurementConstShrdPtr> &measurements = meas->measurements();
    const string sampleNumStr = m_displaySampleNumEdit->text().toUTF8();
    size_t sampleNum = 0;
    if( m_displaySampleDiv->isEnabled() && measurements.size()>1 )
    {
      sampleNum = std::stoull( sampleNumStr );
      if( sampleNum > 0 )
        sampleNum -= 1;
    }

    if( sampleNum > measurements.size() )
      throw runtime_error("");

    sample = measurements.at( sampleNum );
  }catch(...)
  {
    sample.reset();  //not necessary
  }

  return sample;
}//currentMeasurment() const


void SpecFileSummary::handleFieldUpdate( EditableFields field )
{
  const SpectrumType type = SpectrumType(m_spectraGroup->checkedId());
  std::shared_ptr<SpecMeas> meas = m_specViewer->measurment( type );
  std::shared_ptr<const Measurement> sample = currentMeasurment();

  if( !sample || !meas )
  {
    passMessage( "No spectrum to update", "", WarningWidget::WarningMsgInfo );
    return;
  }//if( !sample )

  m_reloadSpectrum->enable();

  switch( field )
  {
    case kDisplayedLiveTime:
    {
      try
      {
        const string textStr = m_displayedLiveTime->text().toUTF8();
        const float newLiveTime = static_cast<float>(PhysicalUnits::stringToTimeDuration( textStr ));
        if( newLiveTime < 0.0 )
          throw runtime_error( "Live time must be zero or greater" );
        
        const float oldLiveTime = sample->live_time();
        if( newLiveTime == oldLiveTime )
          return;
        meas->set_live_time( newLiveTime, sample );
      }catch( exception &e )
      {
        passMessage( e.what(), "SpecFileSummary", WarningWidget::WarningMsgHigh );
        
        char text[32];
        snprintf( text, sizeof(text), "%.2f s", sample->live_time() );
        m_displayedLiveTime->setText( text );
      }//try / catch
      break;
    }//case kDisplayedLiveTime:

    case kDisplayedRealTime:
    {
      try
      {
        const string textStr = m_displayedRealTime->text().toUTF8();
        const float newRealTime = static_cast<float>( PhysicalUnits::stringToTimeDuration(textStr) );
        if( newRealTime < 0.0 )
          throw runtime_error( "Real time must be zero or greater" );
        
        const float oldRealTime = sample->real_time();
        if( newRealTime == oldRealTime )
          return;
        meas->set_real_time( newRealTime, sample );
      }catch( exception &e )
      {
        passMessage( e.what(), "SpecFileSummary",WarningWidget::WarningMsgHigh );
        
        char text[32];
        snprintf( text, sizeof(text), "%.2f", sample->real_time() );
        m_displayedRealTime->setText( text );
      }//try / catch
      break;
    }//case kDisplayedRealTime:

    case kTimeStamp:
    {
      const string newDateStr = m_timeStamp->text().toUTF8();
      boost::posix_time::ptime newDate = UtilityFunctions::time_from_string( newDateStr.c_str() );

      if( newDate.is_special() )
      {
        passMessage( "Error converting '" + newDateStr
                     + string("' to a date/time string"),
                     "SpecFileSummary",WarningWidget::WarningMsgHigh );
        stringstream timeStampStrm;
        timeStampStrm << sample->start_time();
        m_timeStamp->setText( timeStampStrm.str() );
        return;
      }//if( newDate.is_special() )

      meas->set_start_time( newDate, sample );
      break;
    }//case kTimeStamp:

    case kPosition:
    {
      double longitude = coordVal( m_longitude->text() );
      double latitude = coordVal( m_latitude->text() );
      
//      cerr << "Long '" << m_longitude->text().toUTF8() << "'--->" << longitude << endl;
//      cerr << "Lat  '" << m_latitude->text().toUTF8() << "'--->" << latitude << endl;
      
      if( fabs(longitude) > 180.0 )
        longitude = -999.9;
      if( fabs(latitude) > 90.0 )
        latitude = -999.9;
      
      bool converror = false;
      if( !m_longitude->text().empty() )
      {
        if( fabs(longitude)>999.0 && fabs(sample->longitude())<999.0 )
        {
          converror = true;
          longitude = sample->longitude();
          char buffer[32];
          snprintf( buffer, sizeof(buffer), "%.6f", longitude );
          m_longitude->setText( buffer );
        }else if( fabs(longitude)>999.0 )
        {
          converror = true;
          m_longitude->setText( "" );
        }
      }//if( !m_longitude->text().empty() )
      
      if( !m_latitude->text().empty() )
      {
        if( fabs(latitude)>999.0 && fabs(sample->latitude())<999.0 )
        {
          converror = true;
          latitude = sample->latitude();
          char buffer[32];
          snprintf( buffer, sizeof(buffer), "%.6f", latitude );
          m_latitude->setText( buffer );
        }else if( fabs(latitude)>999.0 )
        {
          converror = true;
          m_latitude->setText( "" );
        }
      }//if( !m_latitude->text().empty() )
      
      if( converror )
        passMessage( "Error converting Long/Lat to valid float",
                     "", WarningWidget::WarningMsgHigh );
      
      
      const string newDateStr = m_gpsTimeStamp->text().toUTF8();
      boost::posix_time::ptime newDate = UtilityFunctions::time_from_string( newDateStr.c_str() );
      
      if( newDate.is_special() && newDateStr.size() )
      {
        passMessage( "Error converting '" + newDateStr
                     + string("' to a date/time string"),
                     "",WarningWidget::WarningMsgHigh );
        newDate = sample->position_time();
        stringstream timeStampStrm;
        timeStampStrm << newDate;
        m_timeStamp->setText( timeStampStrm.str() );
      }//if( newDate.is_special() )

#if( USE_GOOGLE_MAP )
      if( fabs(latitude)<999.0 && fabs(longitude)<999.0 )
        m_showMapButton->enable();
#endif
      
      meas->set_position( longitude, latitude, newDate, sample );
      
      break;
    }//case kPosition:
      
    case kDescription:
    {
      const string newtitle = m_title->text().toUTF8();
      meas->set_title( newtitle, sample );
      break;
    }//case kDescription:
      
    case kSourceType:
    {
      const int index = m_source->currentIndex();
      Measurement::SourceType type = Measurement::UnknownSourceType;
      if( index >= 0 )
        type = Measurement::SourceType( m_source->currentIndex() );
      
      meas->set_source_type( type, sample );
      break;
    }//case kSourceType:
      
    case kMeasurmentRemarks:
    {
      const string newRemark = m_measurmentRemarks->text().toUTF8();
      vector<string> newRemarks;
      boost::algorithm::split( newRemarks, newRemark,
                               boost::is_any_of( "\r\n" ),
                               boost::token_compress_on );
      meas->set_remarks( newRemarks, sample );
      break;
    }//case kMeasurmentRemarks:


    case kFileRemarks:
    {
      const string newRemark = m_fileRemarks->text().toUTF8();
      vector<string> newRemarks;
      boost::algorithm::split( newRemarks, newRemark,
                               boost::is_any_of( "\r\n" ),
                               boost::token_compress_on );
      meas->set_remarks( newRemarks );
      break;
    }//case kFileRemarks:

    case kFilename:
      meas->set_filename( m_filename->text().toUTF8() );
      break;

    case kUuid:
      meas->set_uuid( m_uuid->text().toUTF8() );
      break;

    case kLaneNumber:
    {
      try
      {
        meas->set_lane_number( std::stoi( m_laneNumber->text().toUTF8() ) );
      }catch(...)
      {
        m_laneNumber->setText( std::to_string( meas->lane_number() ) );
      }
      break;
    }//case kLaneNumber:

    case kMeasurement_location_name:
      meas->set_measurement_location_name( m_measurement_location_name->text().toUTF8() );
      break;

    case kInspection:
      meas->set_inspection( m_inspection->text().toUTF8() );
      break;

    case kInstrument_type:
      meas->set_instrument_type( m_instrument_type->text().toUTF8() );
      break;

    case kManufacturer:
      meas->set_manufacturer( m_manufacturer->text().toUTF8() );
      break;

    case kInstrument_model:
      meas->set_instrument_model( m_instrument_model->text().toUTF8() );
      break;

    case kInstrument_id:
      meas->set_instrument_id( m_instrument_id->text().toUTF8() );
      break;
  }//switch( field )
}//void handleFieldUpdate( EditableFields field )


void SpecFileSummary::handleSpectrumChange( SpectrumType type,
                                            std::shared_ptr<SpecMeas> meas,
                                            std::set<int> displaySample )
{
  const SpectrumType display_type = SpectrumType(m_spectraGroup->checkedId());
  
  if( type != display_type )
  {
    WRadioButton *button = m_spectraGroup->button( type );
    if( button )
      button->setEnabled( !!meas );
  }else
  {
    m_reloadSpectrum->disable();
    updateDisplayFromMemory();
  }
}//void handleSpectrumChange(...)


void SpecFileSummary::reloadCurrentSpectrum()
{
  const SpectrumType type = SpectrumType(m_spectraGroup->checkedId());
  m_specViewer->reloadCurrentSpectrum( type );
  m_reloadSpectrum->disable();
}//void reloadCurrentSpectrum()


void SpecFileSummary::handleSpectrumTypeChanged()
{
  const SpectrumType type = SpectrumType(m_spectraGroup->checkedId());

  std::shared_ptr<const SpecMeas> meas = m_specViewer->measurment( type );
  if( !meas )
  {
    m_displaySampleNumEdit->setText( "" );
    m_displaySampleNumValidator->setRange( 0, 0 );
  }else
  {
    const vector<MeasurementConstShrdPtr> &measurements = meas->measurements();

    if( measurements.size() )
    {
      m_displaySampleNumValidator->setRange( 1, static_cast<int>(measurements.size()) );
      m_displaySampleNumEdit->setText( "1" );
    }else
    {
      m_displaySampleNumValidator->setRange( 0, 0 );
      m_displaySampleNumEdit->setText( "" );
    }
  }//if( !meas ) /else

  m_reloadSpectrum->disable();

  updateDisplayFromMemory();
}//void handleSpectrumTypeChanged( Wt::WRadioButton *button )



void SpecFileSummary::handleAllowModifyStatusChange()
{
  const AllowModifyStatus status = AllowModifyStatus(m_allowEditGroup->checkedId());

  bool disable = true;
  switch( status )
  {
    case kAllowModify:
      disable = false;
    break;

    case kDontAllowModify:
      disable = true;
    break;
  }//switch( status )

  m_displayedLiveTime->setDisabled( disable );
  m_displayedRealTime->setDisabled( disable );
  m_timeStamp->setDisabled( disable );
  m_measurmentRemarks->setDisabled( disable );
  m_fileRemarks->setDisabled( disable );

  m_longitude->setDisabled( disable );
  m_latitude->setDisabled( disable );
  m_gpsTimeStamp->setDisabled( disable );
  
  m_title->setDisabled( disable );
  m_source->setDisabled( disable );
  
  m_filename->setDisabled( disable );
  m_uuid->setDisabled( disable );
  m_laneNumber->setDisabled( disable );
  m_measurement_location_name->setDisabled( disable );
  m_ana_results->allowModifiable( !disable );
  m_inspection->setDisabled( disable );
  m_instrument_type->setDisabled( disable );
  m_manufacturer->setDisabled( disable );
  m_instrument_model->setDisabled( disable );
  m_instrument_id->setDisabled( disable );
}//void handleAllowModifyStatusChange(...)


void SpecFileSummary::handleUserIncrementSampleNum( bool increment )
{
  const SpectrumType type = SpectrumType(m_spectraGroup->checkedId());

  std::shared_ptr<const SpecMeas> meas = m_specViewer->measurment( type );

  try
  {
    if( !meas )
    {
      m_displaySampleNumValidator->setRange( 0, 0 );
      throw runtime_error("");
    }//if( !meas )

    const vector<MeasurementConstShrdPtr> &measurements = meas->measurements();
    if( measurements.size() )
    {
      m_displaySampleNumValidator->setRange( 1, static_cast<int>(measurements.size()) );
    }else
    {
      m_displaySampleNumValidator->setRange( 0, 0 );
      throw runtime_error( "" );
    }

    size_t prevSpecNum;
    try
    {
      prevSpecNum = std::stoull( m_displaySampleNumEdit->text().toUTF8() );
    }catch(...)
    {
      prevSpecNum = 0;
    }

    if( prevSpecNum > 0 )
      prevSpecNum -= 1;

    size_t nextSpecNum;

    if( increment )
    {
      if( prevSpecNum >= (measurements.size()-1) )
        nextSpecNum = 0;
      else
        nextSpecNum = prevSpecNum + 1;
    }else
    {
      if( prevSpecNum == 0 )
        nextSpecNum = measurements.size()-1;
      else
        nextSpecNum = prevSpecNum - 1;
    }//if( increment ) / else

    m_displaySampleNumEdit->setText( std::to_string(nextSpecNum+1) );
  }catch(...)
  {
    m_displaySampleNumEdit->setText( "" );
  }

  updateMeasurmentFieldsFromMemory();
}//void SpecFileSummary::handleUserIncrementSampleNum( bool increment )


void SpecFileSummary::handleUserChangeSampleNum()
{
  updateMeasurmentFieldsFromMemory();
}//void SpecFileSummary::handleUserChangeSampleNum()
