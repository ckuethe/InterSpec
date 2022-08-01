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

#include <map>

#include "rapidxml/rapidxml.hpp"
#include "rapidxml/rapidxml_print.hpp"
#include "rapidxml/rapidxml_utils.hpp"

#include <Wt/WMenu>
#include <Wt/WText>
#include <Wt/WLabel>
#include <Wt/WPoint>
#include <Wt/WServer>
#include <Wt/WMenuItem>
#include <Wt/WCheckBox>
#include <Wt/WComboBox>
#include <Wt/WIOService>
#include <Wt/WGridLayout>
#include <Wt/WPushButton>
#include <Wt/WStackedWidget>
#include <Wt/WContainerWidget>
#include <Wt/WRegExpValidator>
#include <Wt/WSuggestionPopup>

#include "SpecUtils/SpecFile.h"
#include "SpecUtils/Filesystem.h"
#include "SpecUtils/StringAlgo.h"
#include "SpecUtils/RapidXmlUtils.hpp"

#include "InterSpec/PeakFit.h"
#include "InterSpec/PopupDiv.h"
#include "InterSpec/SpecMeas.h"
#include "InterSpec/AuxWindow.h"
#include "InterSpec/InterSpec.h"
#include "InterSpec/PeakModel.h"
#include "InterSpec/ColorTheme.h"
#include "InterSpec/HelpSystem.h"
#include "InterSpec/RelActCalc.h"
#include "InterSpec/ColorSelect.h"
#include "InterSpec/RelEffChart.h"
#include "InterSpec/InterSpecApp.h"
#include "InterSpec/PeakFitUtils.h"
#include "InterSpec/SimpleDialog.h"
#include "InterSpec/PhysicalUnits.h"
#include "InterSpec/RelActAutoGui.h"
#include "InterSpec/WarningWidget.h"
#include "InterSpec/RelActCalcAuto.h"
#include "InterSpec/RelActTxtResults.h"
#include "InterSpec/NativeFloatSpinBox.h"
#include "InterSpec/DecayDataBaseServer.h"
#include "InterSpec/D3SpectrumDisplayDiv.h"
#include "InterSpec/DetectorPeakResponse.h"
#include "InterSpec/IsotopeNameFilterModel.h"
#include "InterSpec/ReferencePhotopeakDisplay.h"


using namespace Wt;
using namespace std;

namespace
{
  //DeleteOnClosePopupMenu - same class as from D3SpectrumDisplayDiv... should refactor
  class DeleteOnClosePopupMenu : public PopupDivMenu
  {
    bool m_deleteWhenHidden;
  public:
    DeleteOnClosePopupMenu( WPushButton *p, const PopupDivMenu::MenuType t )
      : PopupDivMenu( p, t ), m_deleteWhenHidden( false ) {}
    virtual ~DeleteOnClosePopupMenu(){}
    void markForDelete(){ m_deleteWhenHidden = true; }
    virtual void setHidden( bool hidden, const WAnimation &a = WAnimation() )
    {
      PopupDivMenu::setHidden( hidden, a );
      if( hidden && m_deleteWhenHidden )
        delete this;
    }
  };//class PeakRangePopupMenu

  class RelActAutoEnergyRange : public WContainerWidget
  {
    RelActAutoGui *m_gui;
    Wt::Signal<> m_updated;
    Wt::Signal<> m_remove_energy_range;
    
    NativeFloatSpinBox *m_lower_energy;
    NativeFloatSpinBox *m_upper_energy;
    WComboBox *m_continuum_type;
    WCheckBox *m_force_full_range;
    WPushButton *m_to_individual_rois;
    
    /// Used to track the Highlight region this energy region corresponds to in D3SpectrumDisplayDiv
    size_t m_highlight_region_id;
    
  public:
    RelActAutoEnergyRange( RelActAutoGui *gui, WContainerWidget *parent = nullptr )
    : WContainerWidget( parent ),
      m_gui( gui ),
      m_updated( this ),
      m_remove_energy_range( this ),
      m_lower_energy( nullptr ),
      m_upper_energy( nullptr ),
      m_continuum_type( nullptr ),
      m_force_full_range( nullptr ),
      m_to_individual_rois( nullptr ),
      m_highlight_region_id( 0 )
    {
      addStyleClass( "RelActAutoEnergyRange" );
      
      wApp->useStyleSheet( "InterSpec_resources/GridLayoutHelpers.css" );
      
      const bool showToolTips = InterSpecUser::preferenceValue<bool>( "ShowTooltips", InterSpec::instance() );
      
      WLabel *label = new WLabel( "Lower Energy", this );
      label->addStyleClass( "GridFirstCol GridFirstRow" );
      
      m_lower_energy = new NativeFloatSpinBox( this );
      m_lower_energy->addStyleClass( "GridSecondCol GridFirstRow" );
      label->setBuddy( m_lower_energy );
      
      label = new WLabel( "keV", this );
      label->addStyleClass( "GridThirdCol GridFirstRow" );
      
      label = new WLabel( "Upper Energy", this );
      label->addStyleClass( "GridFirstCol GridSecondRow" );
      
      m_upper_energy = new NativeFloatSpinBox( this );
      m_upper_energy->addStyleClass( "GridSecondCol GridSecondRow" );
      label->setBuddy( m_upper_energy );
      
      label = new WLabel( "keV", this );
      label->addStyleClass( "GridThirdCol GridSecondRow" );
      
      m_lower_energy->valueChanged().connect( this, &RelActAutoEnergyRange::handleEnergyChange );
      m_upper_energy->valueChanged().connect( this, &RelActAutoEnergyRange::handleEnergyChange );
      
      label = new WLabel( "Continuum Type:", this );
      label->addStyleClass( "GridFourthCol GridFirstRow" );
      m_continuum_type = new WComboBox( this );
      m_continuum_type->addStyleClass( "GridFifthCol GridFirstRow" );
      label->setBuddy( m_continuum_type );
      
      // We wont allow "External" here
      for( int i = 0; i < static_cast<int>(PeakContinuum::OffsetType::External); ++i )
      {
        const char *label = PeakContinuum::offset_type_label( PeakContinuum::OffsetType(i) );
        m_continuum_type->addItem( label );
      }//for( loop over PeakContinuum::OffsetType )
      
      m_continuum_type->setCurrentIndex( static_cast<int>(PeakContinuum::OffsetType::Linear) );
      m_continuum_type->changed().connect( this, &RelActAutoEnergyRange::handleContinuumTypeChange );
      
      m_force_full_range = new WCheckBox( "Force full-range", this );
      m_force_full_range->addStyleClass( "GridFourthCol GridSecondRow GridSpanTwoCol" );
      m_force_full_range->checked().connect( this, &RelActAutoEnergyRange::handleForceFullRangeChange );
      m_force_full_range->unChecked().connect( this, &RelActAutoEnergyRange::handleForceFullRangeChange );
      
      WPushButton *removeEnergyRange = new WPushButton( this );
      removeEnergyRange->setStyleClass( "DeleteEnergyRangeOrNuc GridSixthCol GridFirstRow Wt-icon" );
      removeEnergyRange->setIcon( "InterSpec_resources/images/minus_min_black.svg" );
      removeEnergyRange->clicked().connect( this, &RelActAutoEnergyRange::handleRemoveSelf );
      
      m_to_individual_rois = new WPushButton( this );
      m_to_individual_rois->setStyleClass( "ToIndividualRois GridSixthCol GridSecondRow Wt-icon" );
      m_to_individual_rois->setIcon( "InterSpec_resources/images/expand_list.svg" );
      m_to_individual_rois->clicked().connect( boost::bind( &RelActAutoGui::handleConvertEnergyRangeToIndividuals,
                                                           m_gui, static_cast<WWidget *>(this) ) );
      m_to_individual_rois->setHidden( m_force_full_range->isChecked() );
      
      const char *tooltip = "Converts this energy range into individual ROIs, based on which gammas should be"
      " grouped together vs apart (e.g., leaves gammas with overlapping peaks in a single ROI, while if two gammas"
      " are far apart, they will be split into seperate ROIs.";
      HelpSystem::attachToolTipOn( m_to_individual_rois, tooltip, showToolTips );
    }//RelActAutoEnergyRange constructor
    
    
    bool isEmpty() const
    {
      return ((fabs(m_lower_energy->value() - m_upper_energy->value() ) < 1.0)
              || (m_upper_energy->value() <= 0.0));
    }
    
    
    void handleRemoveSelf()
    {
      m_remove_energy_range.emit();
    }//void handleRemoveSelf()
    
    
    void handleContinuumTypeChange()
    {
      m_updated.emit();
    }
    
    
    void handleForceFullRangeChange()
    {
      m_to_individual_rois->setHidden( m_to_individual_rois->isDisabled() || m_force_full_range->isChecked() );
      
      m_updated.emit();
    }
    
    
    void enableSplitToIndividualRanges( const bool enable )
    {
      m_to_individual_rois->setHidden( !enable || m_force_full_range->isChecked() );
      m_to_individual_rois->setEnabled( enable );
    }
    
    
    void handleEnergyChange()
    {
      float lower = m_lower_energy->value();
      float upper = m_upper_energy->value();
      if( lower > upper )
      {
        m_lower_energy->setValue( upper );
        m_upper_energy->setValue( lower );
        
        std::swap( lower, upper );
      }//if( lower > upper )
      
      m_updated.emit();
    }//void handleEnergyChange()
    
    
    void setEnergyRange( float lower, float upper )
    {
      if( lower > upper )
        std::swap( lower, upper );
      
      m_lower_energy->setValue( lower );
      m_upper_energy->setValue( upper );
      
      m_updated.emit();
    }//void setEnergyRange( float lower, float upper )
    
    
    bool forceFullRange() const
    {
      return m_force_full_range->isChecked();
    }
    
    
    void setForceFullRange( const bool force_full )
    {
      if( force_full == m_force_full_range->isChecked() )
        return;
      
      m_force_full_range->setChecked( force_full );
      m_updated.emit();
    }
    
    
    void setContinuumType( const PeakContinuum::OffsetType type )
    {
      const int type_index = static_cast<int>( type );
      if( (type_index < 0)
         || (type_index >= static_cast<int>(PeakContinuum::OffsetType::External)) )
      {
        assert( 0 );
        return;
      }
      
      if( type_index == m_continuum_type->currentIndex() )
        return;
      
      m_continuum_type->setCurrentIndex( type_index );
      m_updated.emit();
    }//void setContinuumType( PeakContinuum::OffsetType type )
    
    
    void setHighlightRegionId( const size_t chart_id )
    {
      m_highlight_region_id = chart_id;
    }
    
    
    size_t highlightRegionId() const
    {
      return m_highlight_region_id;
    }
  
    float lowerEnergy() const
    {
      return m_lower_energy->value();
    }
    
    
    float upperEnergy() const
    {
      return m_upper_energy->value();
    }
    
    
    void setFromRoiRange( const RelActCalcAuto::RoiRange &roi )
    {
      if( roi.continuum_type == PeakContinuum::OffsetType::External )
        throw runtime_error( "setFromRoiRange: External continuum type not supported" );
      
      if( roi.allow_expand_for_peak_width )
        throw runtime_error( "setFromRoiRange: allow_expand_for_peak_width set to true not supported" );
      
      m_lower_energy->setValue( roi.lower_energy );
      m_upper_energy->setValue( roi.upper_energy );
      m_continuum_type->setCurrentIndex( static_cast<int>(roi.continuum_type) );
      m_force_full_range->setChecked( roi.force_full_range );
    }//setFromRoiRange(...)
    
    
    RelActCalcAuto::RoiRange toRoiRange() const
    {
      RelActCalcAuto::RoiRange roi;
      
      roi.lower_energy = m_lower_energy->value();
      roi.upper_energy = m_upper_energy->value();
      roi.continuum_type = PeakContinuum::OffsetType( m_continuum_type->currentIndex() );
      roi.force_full_range = m_force_full_range->isChecked();
      roi.allow_expand_for_peak_width = false; //not currently supported to the GUI, or tested in the calculation code
      
      return roi;
    }//RelActCalcAuto::RoiRange toRoiRange() const
    
    
    Wt::Signal<> &updated()
    {
      return m_updated;
    }
    
    
    Wt::Signal<> &remove()
    {
      return m_remove_energy_range;
    }
  };//class RelActAutoEnergyRange


  class RelActAutoNuclide : public WContainerWidget
  {
    RelActAutoGui *m_gui;
    
    WLineEdit *m_nuclide_edit;
    WLabel *m_age_label;
    WLineEdit *m_age_edit;
    WCheckBox *m_fit_age;
    ColorSelect *m_color_select;
    
    Wt::Signal<> m_updated;
    Wt::Signal<> m_remove;
    
  public:
    RelActAutoNuclide( RelActAutoGui *gui, WContainerWidget *parent = nullptr )
    : WContainerWidget( parent ),
    m_gui( gui ),
    m_nuclide_edit( nullptr ),
    m_age_label( nullptr ),
    m_age_edit( nullptr ),
    m_fit_age( nullptr ),
    m_color_select( nullptr ),
    m_updated( this ),
    m_remove( this )
    {
      addStyleClass( "RelActAutoNuclide" );
      
      const bool showToolTips = InterSpecUser::preferenceValue<bool>( "ShowTooltips", InterSpec::instance() );
      
      WLabel *label = new WLabel( "Nuclide:", this );
      m_nuclide_edit = new WLineEdit( "", this );
      m_nuclide_edit->setAutoComplete( false );
#if( BUILD_AS_OSX_APP || IOS )
      m_nuclide_edit->setAttributeValue( "autocorrect", "off" );
      m_nuclide_edit->setAttributeValue( "spellcheck", "off" );
#endif
      label->setBuddy( m_nuclide_edit );
      
      m_nuclide_edit->changed().connect( this, &RelActAutoNuclide::handleIsotopeChange );
      
      // If we are typing in this box, we want to let app-hotkeys propagate up, but not arrow keys and
      //  stuff
      const string jsAppKeyDownFcn = wApp->javaScriptClass() + ".appKeyDown";
      const string keyDownJs = "function(s1,e1){"
      "if(e1 && e1.ctrlKey && e1.key && " + jsAppKeyDownFcn + ")"
      + jsAppKeyDownFcn + "(e1);"
      "}";
      m_nuclide_edit->keyWentDown().connect( keyDownJs );
      
      const char *tooltip = "ex. <b>U235</b>, <b>235 Uranium</b>"
      ", <b>U-235m</b> (meta stable state)"
      ", <b>Cs137</b>, etc.";
      HelpSystem::attachToolTipOn( m_nuclide_edit, tooltip, showToolTips );
      
      string replacerJs, matcherJs;
      IsotopeNameFilterModel::replacerJs( replacerJs );
      IsotopeNameFilterModel::nuclideNameMatcherJs( matcherJs );
      IsotopeNameFilterModel *isoSuggestModel = new IsotopeNameFilterModel( this );
      isoSuggestModel->excludeXrays( true );
      isoSuggestModel->excludeEscapes( true );
      isoSuggestModel->excludeReactions( true );
      
      WSuggestionPopup *nuclideSuggest = new WSuggestionPopup( matcherJs, replacerJs, this );
#if( WT_VERSION < 0x3070000 ) //I'm not sure what version of Wt "wtNoReparent" went away.
      nuclideSuggest->setJavaScriptMember("wtNoReparent", "true");
#endif
      nuclideSuggest->setMaximumSize( WLength::Auto, WLength(15, WLength::FontEm) );
      nuclideSuggest->setWidth( WLength(70, Wt::WLength::Unit::Pixel) );
      
      IsotopeNameFilterModel::setQuickTypeFixHackjs( nuclideSuggest );
      
      isoSuggestModel->filter( "" );
      nuclideSuggest->setFilterLength( -1 );
      nuclideSuggest->setModel( isoSuggestModel );
      nuclideSuggest->filterModel().connect( isoSuggestModel, &IsotopeNameFilterModel::filter );
      nuclideSuggest->forEdit( m_nuclide_edit, WSuggestionPopup::Editing );  // | WSuggestionPopup::DropDownIcon
      
      m_age_label = new WLabel( "Age:", this );
      m_age_edit = new WLineEdit( "", this );
      m_age_label->setBuddy( m_age_edit );
      
      WRegExpValidator *validator = new WRegExpValidator( PhysicalUnits::sm_timeDurationHalfLiveOptionalRegex, this );
      validator->setFlags(Wt::MatchCaseInsensitive);
      m_age_edit->setValidator(validator);
      m_age_edit->setAutoComplete( false );
      m_age_edit->changed().connect( this, &RelActAutoNuclide::handleAgeChange );
      
      m_fit_age = new WCheckBox( "Fit Age", this );
      m_fit_age->setWordWrap( false );
      m_fit_age->checked().connect( this, &RelActAutoNuclide::handleFitAgeChange );
      m_fit_age->unChecked().connect( this, &RelActAutoNuclide::handleFitAgeChange );
      
      WContainerWidget *spacer = new WContainerWidget( this );
      spacer->addStyleClass( "RelActAutoNuclideSpacer" );
      
      
      m_color_select = new ColorSelect( ColorSelect::PrefferNative, this );
      m_color_select->setColor( WColor("#FF6633") );
      
      m_color_select->cssColorChanged().connect( this, &RelActAutoNuclide::handleColorChange );
      
      
      WPushButton *removeEnergyRange = new WPushButton( this );
      removeEnergyRange->setStyleClass( "DeleteEnergyRangeOrNuc Wt-icon" );
      removeEnergyRange->setIcon( "InterSpec_resources/images/minus_min_black.svg" );
      removeEnergyRange->clicked().connect( this, &RelActAutoNuclide::handleRemoveSelf );
    }//RelActAutoNuclide
    
    
    void handleIsotopeChange()
    {
      const SandiaDecay::Nuclide *nuc = nuclide();
      if( !nuc )
      {
        const string nucstr = m_nuclide_edit->text().toUTF8();
        
        m_fit_age->setUnChecked();
        m_fit_age->hide();
        
        if( !nucstr.empty() )
          passMessage( nucstr + " is not a valid nuclide.", "", WarningWidget::WarningMsgHigh );
        
        m_updated.emit();
        return;
      }//if( !nuc )
      
      if( IsInf(nuc->halfLife) )
      {
        const string nucstr = m_nuclide_edit->text().toUTF8();
        passMessage( nucstr + " is a stable nuclide.", "", WarningWidget::WarningMsgHigh );
        
        m_nuclide_edit->setText( "" );
        m_nuclide_edit->validate();
        m_fit_age->setUnChecked();
        m_fit_age->hide();
        
        m_updated.emit();
        return;
      }//if( IsInf(nuc->halfLife) )
      
      const bool age_is_fittable = !PeakDef::ageFitNotAllowed( nuc );
      
      if( nuc->decaysToStableChildren() )
      {
        m_age_edit->setText( "0y" );
      }else
      {
        string agestr;
        PeakDef::defaultDecayTime( nuc, &agestr );
        m_age_edit->setText( agestr );
      }
      
      m_age_label->setHidden( !age_is_fittable );
      m_age_edit->setHidden( !age_is_fittable );
      m_fit_age->setHidden( !age_is_fittable );
      m_fit_age->setChecked( false );
      
      bool haveFoundColor = false;
      
      // Check if user is showing reference lines for this nuclide, and if so, use that color
      if( !haveFoundColor )
      {
        vector<ReferenceLineInfo> reflines;
        const ReferencePhotopeakDisplay *refdisp = InterSpec::instance()->referenceLinesWidget();
        if( refdisp )
          reflines = refdisp->showingNuclides();
        
        for( const auto &refline : reflines )
        {
          if( (refline.nuclide == nuc) && !refline.lineColor.isDefault() )
          {
            haveFoundColor = true;
            m_color_select->setColor( refline.lineColor );
            break;
          }//if( we found a match )
        }//for( const auto &refline : reflines )
        
        // TODO: look in ReferencePhotopeakDisplay cache of previous colors
      }//if( !haveFoundColor )
      
      // Check for user-fit peaks
      if( !haveFoundColor )
      {
        PeakModel *peakmodel = InterSpec::instance()->peakModel();
        if( peakmodel && peakmodel->peaks() )
        {
          for( auto peakshrdptr : *peakmodel->peaks() )
          {
            if( !peakshrdptr )
              continue;
            
            if( (peakshrdptr->parentNuclide() == nuc) && !peakshrdptr->lineColor().isDefault() )
            {
              haveFoundColor = true;
              m_color_select->setColor( peakshrdptr->lineColor() );
              break;
            }//if( we found a matching nuclide )
          }//for( loop over user peaks )
        }//if( PeakModel is valid  - should always be )
      }//if( !haveFoundColor )
      
      // Finally check
      if( !haveFoundColor )
      {
        shared_ptr<const ColorTheme> theme = InterSpec::instance()->getColorTheme();
        if( theme )
        {
          const map<string,WColor> &defcolors = theme->referenceLineColorForSources;
          const auto pos = defcolors.find( nuc->symbol );
          if( pos != end(defcolors) )
            m_color_select->setColor( pos->second );
        }//if( theme )
      }//if( !haveFoundColor )
      
      if( !haveFoundColor )
      {
        // TODO: create a cache of previous user-selected colors
      }//if( !haveFoundColor )
      
      m_updated.emit();
    }//void handleIsotopeChange()
    
    
    void setFitAgeVisible( bool visible, bool do_fit )
    {
      if( visible )
      {
        m_fit_age->setHidden( false );
        m_fit_age->setChecked( do_fit );
      }else
      {
        m_fit_age->setHidden( true );
        m_fit_age->setChecked( false );
      }
    }//setFitAgeVisible(...)
    
    
    void setAgeDisabled( bool disabled )
    {
      m_fit_age->setDisabled( disabled );
      m_age_edit->setDisabled( disabled );
    }//void setAgeDisabled( bool disabled )
    
    
    void setAge( const string &age )
    {
      m_age_edit->setValueText( WString::fromUTF8(age) );
    }
    
    void setNuclideEditFocus()
    {
      m_nuclide_edit->setFocus();
    }
    
    void handleAgeChange()
    {
      m_updated.emit();
    }//void handleAgeChange()
    
    
    void handleFitAgeChange()
    {
      
      m_updated.emit();
    }
    
    
    void handleColorChange()
    {
      m_updated.emit();
    }
    
    /** Will return nullptr if invalid text entered. */
    const SandiaDecay::Nuclide *nuclide() const
    {
      const string nucstr = m_nuclide_edit->text().toUTF8();
      const SandiaDecay::SandiaDecayDataBase * const db = DecayDataBaseServer::database();
      
      return db->nuclide( nucstr );
    }
    
    WColor color() const
    {
      return m_color_select->color();
    }
    
    void setColor( const WColor &color )
    {
      m_color_select->setColor( color );
    }
    
    double age() const
    {
      const SandiaDecay::Nuclide * const nuc = nuclide();
      if( !nuc )
        return 0.0;
    
      if( m_age_edit->isHidden() || m_age_edit->text().empty() )
        return PeakDef::defaultDecayTime( nuc );
      
      double age = 0.0;
      try
      {
        const string agestr = m_age_edit->text().toUTF8();
        age = PhysicalUnits::stringToTimeDurationPossibleHalfLife( agestr, nuc->halfLife );
      }catch( std::exception & )
      {
        age = PeakDef::defaultDecayTime( nuc );
      }//try / catch
      
      return age;
    }//double age() const
    
    
    RelActCalcAuto::NucInputInfo toNucInputInfo() const
    {
      const SandiaDecay::Nuclide * const nuc = nuclide();
      if( !nuc )
        throw runtime_error( "No valid nuclide" );
      
      RelActCalcAuto::NucInputInfo nuc_info;

      nuc_info.nuclide = nuc;
      nuc_info.age = age(); // Must not be negative.
      nuc_info.fit_age = m_fit_age->isChecked();
      
      // TODO: Not implemented: vector<double> gammas_to_exclude;
      //nuc_info.gammas_to_exclude = ;
      
      nuc_info.peak_color_css = m_color_select->color().cssText();

      return nuc_info;
    }//RelActCalcAuto::RoiRange toRoiRange() const
    
    
    void fromNucInputInfo( const RelActCalcAuto::NucInputInfo &info )
    {
      if( !info.nuclide )
      {
        m_nuclide_edit->setText( "" );
        if( !info.peak_color_css.empty() )
          m_color_select->setColor( WColor(info.peak_color_css) );
        
        m_age_label->hide();
        m_age_edit->hide();
        m_age_edit->setText( "0s" );
        m_fit_age->setUnChecked();
        m_fit_age->hide();
        
        m_updated.emit();
        
        return;
      }//if( !info.nuclide )
      
      m_nuclide_edit->setText( WString::fromUTF8(info.nuclide->symbol) );
      handleIsotopeChange();
      
      if( !info.peak_color_css.empty() )
        m_color_select->setColor( WColor(info.peak_color_css) );
      
      const SandiaDecay::Nuclide * const nuc = nuclide();
      
      const bool age_is_fittable = !PeakDef::ageFitNotAllowed(nuc);
      m_age_label->setHidden( !age_is_fittable );
      m_age_edit->setHidden( !age_is_fittable );
      m_fit_age->setHidden( !age_is_fittable );
      m_fit_age->setChecked( age_is_fittable && info.fit_age );
      
      if( !age_is_fittable || (info.age < 0.0) )
      {
        string agestr = "0s";
        if( nuc )
          PeakDef::defaultDecayTime( nuc, &agestr );
        m_age_edit->setText( WString::fromUTF8(agestr) );
      }else
      {
        const string agestr = PhysicalUnits::printToBestTimeUnits(info.age);
        m_age_edit->setText( WString::fromUTF8(agestr) );
      }
      // Not currently supported: info.gammas_to_exclude -> vector<double>;
    }//void fromNucInputInfo( const RelActCalcAuto::NucInputInfo &info )
    
    
    void handleRemoveSelf()
    {
      m_remove.emit();
    }
    
    
    Wt::Signal<> &updated()
    {
      return m_updated;
    }
    
    
    Wt::Signal<> &remove()
    {
      return m_remove;
    }
  };//class RelActAutoNuclide

  
  
  
  class RelActFreePeak : public WContainerWidget
  {
    RelActAutoGui *m_gui;
    NativeFloatSpinBox *m_energy;
    WCheckBox *m_fwhm_constrained;
    WCheckBox *m_apply_energy_cal;
    WText *m_invalid;
    
    Wt::Signal<> m_updated;
    Wt::Signal<> m_remove;
    
  public:
    RelActFreePeak( RelActAutoGui *gui, WContainerWidget *parent = nullptr )
    : WContainerWidget( parent ),
    m_gui( gui ),
    m_energy( nullptr ),
    m_fwhm_constrained( nullptr ),
    m_apply_energy_cal( nullptr ),
    m_updated( this ),
    m_remove( this )
    {
      addStyleClass( "RelActFreePeak" );
      
      const bool showToolTips = InterSpecUser::preferenceValue<bool>( "ShowTooltips", InterSpec::instance() );
      
      WLabel *label = new WLabel( "Energy", this );
      label->addStyleClass( "GridFirstCol GridFirstRow" );
      
      m_energy = new NativeFloatSpinBox( this );
      label->setBuddy( m_energy );
      m_energy->valueChanged().connect( this, &RelActFreePeak::handleEnergyChange );
      m_energy->addStyleClass( "GridSecondCol GridFirstRow" );
      
      // Things are a little cramped if we include the keV
      //label = new WLabel( "keV", this );
      //label->addStyleClass( "GridThirdCol GridFirstRow" );
      
      WPushButton *removeFreePeak = new WPushButton( this );
      removeFreePeak->setStyleClass( "DeleteEnergyRangeOrNuc Wt-icon" );
      removeFreePeak->setIcon( "InterSpec_resources/images/minus_min_black.svg" );
      removeFreePeak->clicked().connect( this, &RelActFreePeak::handleRemoveSelf );
      removeFreePeak->addStyleClass( "GridThirdCol GridFirstRow" );
      
      m_fwhm_constrained = new WCheckBox( "Constrain FWHM", this );
      m_fwhm_constrained->setChecked( true );
      m_fwhm_constrained->checked().connect( this, &RelActFreePeak::handleFwhmConstrainChanged );
      m_fwhm_constrained->unChecked().connect( this, &RelActFreePeak::handleFwhmConstrainChanged );
      m_fwhm_constrained->addStyleClass( "FreePeakConstrain GridFirstCol GridSecondRow GridSpanThreeCol" );
      
      const char *tooltip = "When checked, this peak will be constrained to the FWHM functional form gamma and x-ray"
      "peaks are normally constrained to.<br />"
      "When un-checked, the FWHM for this peak will be fit from the data, independent of all other peak widths.<br />"
      "Un-checking this option is useful for annihilation and reaction photopeaks.";
      HelpSystem::attachToolTipOn( m_fwhm_constrained, tooltip, showToolTips );
      
      
      m_apply_energy_cal = new WCheckBox( "True Energy", this );
      m_apply_energy_cal->setChecked( true );
      m_apply_energy_cal->checked().connect( this, &RelActFreePeak::handleApplyEnergyCalChanged );
      m_apply_energy_cal->unChecked().connect( this, &RelActFreePeak::handleApplyEnergyCalChanged );
      m_apply_energy_cal->addStyleClass( "FreePeakConstrain GridFirstCol GridThirdRow GridSpanThreeCol" );
      tooltip = "Check this option if the peak is for a gamma of known energy.<br />"
      "Un-check this option if this peak is an observed peak in the spectrum with unknown true energy.";
      HelpSystem::attachToolTipOn( m_apply_energy_cal, tooltip, showToolTips );
      
      
      m_invalid = new WText( "Not in a ROI.", this );
      m_invalid->addStyleClass( "InvalidFreePeakEnergy GridFirstCol GridFourthRow GridSpanThreeCol" );
      m_invalid->hide();
    }//RelActFreePeak constructor
    
    
    float energy() const
    {
      return m_energy->value();
    }
    
    
    float setEnergy( const float energy )
    {
      m_energy->setValue( energy );
      m_updated.emit();
    }
    
    
    bool fwhmConstrained() const
    {
      return m_fwhm_constrained->isChecked();
    }
    
    
    void setFwhmConstrained( const bool constrained )
    {
      m_fwhm_constrained->setChecked( constrained );
      m_updated.emit();
    }
    
    
    bool applyEnergyCal() const
    {
      return (m_apply_energy_cal->isVisible() && m_apply_energy_cal->isChecked());
    }
    
    
    void setApplyEnergyCal( const bool apply )
    {
      m_apply_energy_cal->setChecked( apply );
    }
    
    
    void setInvalidEnergy( const bool invalid )
    {
      if( invalid != m_invalid->isVisible() )
        m_invalid->setHidden( !invalid );
    }
    
    
    void handleRemoveSelf()
    {
      m_remove.emit();
    }
    
    
    void handleEnergyChange()
    {
      m_updated.emit();
    }//
    
    
    void handleFwhmConstrainChanged()
    {
      m_updated.emit();
    }
    
    
    void handleApplyEnergyCalChanged()
    {
      m_updated.emit();
    }
    
    
    void setApplyEnergyCalVisible( const bool visible )
    {
      if( m_apply_energy_cal->isVisible() != visible )
        m_apply_energy_cal->setHidden( !visible );
    }
    
    
    Wt::Signal<> &updated()
    {
      return m_updated;
    }
    
    
    Wt::Signal<> &remove()
    {
      return m_remove;
    }
  };//RelActFreePeak
}//namespace


std::pair<RelActAutoGui *,AuxWindow *> RelActAutoGui::createWindow( InterSpec *viewer  )
{
  assert( viewer );
  
  AuxWindow *window = nullptr;
  RelActAutoGui *disp = nullptr;
  
  try
  {
    disp = new RelActAutoGui( viewer );
    
    window = new AuxWindow( "Relative Act. Isotopics" );
    // We have to set minimum size before calling setResizable, or else Wt's Resizable.js functions
    //  will be called first, which will then default to using the initial size as minimum allowable
    window->setMinimumSize( 800, 480 );
    window->setResizable( true );
    window->contents()->setOffsets(WLength(0,WLength::Pixel));
    
    disp->setHeight( WLength(100, WLength::Percentage) );
    disp->setWidth( WLength(100, WLength::Percentage) );
    
    window->contents()->addWidget( disp );
    
    //window->stretcher()->addWidget( disp, 0, 0 );
    //window->stretcher()->setContentsMargins(0,0,0,0);
    //    window->footer()->resize(WLength::Auto, WLength(50.0));
    
    WPushButton *closeButton = window->addCloseButtonToFooter();
    closeButton->clicked().connect(window, &AuxWindow::hide);
    
    AuxWindow::addHelpInFooter( window->footer(), "rel-act-dialog" );
    
    //window->rejectWhenEscapePressed();
    
    // TODO: Similar to activity shielding fit, should store the current widget state in the SpecMeas
    
    double windowWidth = viewer->renderedWidth();
    double windowHeight = viewer->renderedHeight();
    
    if( (windowHeight > 110) && (windowWidth > 110) )
    {
      if( !viewer->isPhone() )
      {
        // A size of 1050px by 555px is about the smallest that renders everything nicely.
        if( (windowWidth > (1050.0/0.8)) && (windowHeight > (555.0/0.8)) )
        {
          windowWidth = 0.8*windowWidth;
          windowHeight = 0.8*windowHeight;
          window->resizeWindow( windowWidth, windowHeight );
        }else
        {
          windowWidth = 0.9*windowWidth;
          windowHeight = 0.9*windowHeight;
          window->resizeWindow( windowWidth, windowHeight );
        }
      }//if( !viewer->isPhone() )
    }else if( !viewer->isPhone() )
    {
      //When loading an application state that is showing this window, we may
      //  not know the window size (e.g., windowWidth==windowHeight==0), so
      //  instead skip giving the initial size hint, and instead size things
      //  client side (maybe we should just do this always?)
      window->resizeScaledWindow( 0.90, 0.90 );
    }
    
    window->centerWindow();
    window->finished().connect( viewer, &InterSpec::closeShieldingSourceFitWindow );
    
    window->WDialog::setHidden(false);
    window->show();
    window->centerWindow();
  }catch( std::exception &e )
  {
    passMessage( "Error creating Relative Act. Isotopics tool: " + string(e.what()),
                "", WarningWidget::WarningMsgHigh );
    
    if( disp )
      delete disp;
    disp = nullptr;
    
    if( window )
      AuxWindow::deleteAuxWindow( window );
    window = nullptr;
  }//try / catch
  
  return make_pair( disp, window );
}//createWindow( InterSpec *viewer  )




RelActAutoGui::RelActAutoGui( InterSpec *viewer, Wt::WContainerWidget *parent )
: WContainerWidget( parent ),
  m_render_flags( 0 ),
  m_default_par_sets_dir( "" ),
  m_user_par_sets_dir( "" ),
  m_interspec( viewer ),
  m_foreground( nullptr ),
  m_background( nullptr ),
  m_background_sf( 0.0 ),
  m_status_indicator( nullptr ),
  m_spectrum( nullptr ),
  m_peak_model( nullptr ),
  m_rel_eff_chart( nullptr ),
  m_txt_results( nullptr ),
  m_upper_menu( nullptr ),
  m_presets( nullptr ),
  m_loading_preset( false ),
  m_current_preset_index( -1 ),
  m_error_msg( nullptr ),
  m_rel_eff_eqn_form( nullptr ),
  m_rel_eff_eqn_order( nullptr ),
  m_fwhm_eqn_form( nullptr ),
  m_fit_energy_cal( nullptr ),
  m_background_subtract( nullptr ),
  m_same_z_age( nullptr ),
  m_u_pu_by_correlation( nullptr ),
  m_show_free_peak( nullptr ),
  m_free_peaks_container( nullptr ),
//  m_u_pu_data_source( nullptr ),
  m_nuclides( nullptr ),
  m_energy_ranges( nullptr ),
  m_free_peaks( nullptr ),
  m_is_calculating( false ),
  m_cancel_calc{},
  m_solution{}
{
  assert( m_interspec );
  if( !m_interspec )
    throw runtime_error( "RelActAutoGui: requires pointer to InterSpec" );
  
  wApp->useStyleSheet( "InterSpec_resources/RelActAutoGui.css" );
  wApp->useStyleSheet( "InterSpec_resources/GridLayoutHelpers.css" );
  
  addStyleClass( "RelActAutoGui" );
  
  const bool showToolTips = InterSpecUser::preferenceValue<bool>( "ShowTooltips", m_interspec );
  
  WContainerWidget *upper_div = new WContainerWidget( this );
  upper_div->addStyleClass( "RelActAutoUpperArea" );
  
  WStackedWidget *upper_stack = new WStackedWidget();
  upper_stack->addStyleClass( "UpperStack" );
  WAnimation animation(Wt::WAnimation::Fade, Wt::WAnimation::Linear, 200);
  upper_stack->setTransitionAnimation( animation, true );
  
  m_upper_menu = new WMenu( upper_stack, Wt::Vertical, upper_div );
  m_upper_menu->addStyleClass( "UpperMenu LightNavMenu" );
  upper_div->addWidget( upper_stack );

  
  m_spectrum = new D3SpectrumDisplayDiv();
  m_spectrum->setCompactAxis( true );
  m_spectrum->disableLegend();
  m_spectrum->setXAxisTitle( "Energy (keV)" );
  m_spectrum->setYAxisTitle( "Counts" );
  m_interspec->colorThemeChanged().connect( boost::bind( &D3SpectrumDisplayDiv::applyColorTheme, m_spectrum, boost::placeholders::_1 ) );
  m_spectrum->applyColorTheme( m_interspec->getColorTheme() );
  
  m_peak_model = new PeakModel( m_spectrum );
  m_peak_model->setNoSpecMeasBacking();
  
  m_spectrum->setPeakModel( m_peak_model );
  
  m_spectrum->existingRoiEdgeDragUpdate().connect( this, &RelActAutoGui::handleRoiDrag );
  m_spectrum->dragCreateRoiUpdate().connect( this, &RelActAutoGui::handleCreateRoiDrag );
  m_spectrum->rightClicked().connect( this, &RelActAutoGui::handleRightClick );
  m_spectrum->shiftKeyDragged().connect( this, &RelActAutoGui::handleShiftDrag );
  m_spectrum->doubleLeftClick().connect( this, &RelActAutoGui::handleDoubleLeftClick );
  
  m_rel_eff_chart = new RelEffChart();
  m_txt_results = new RelActTxtResults();
  
  WMenuItem *item = new WMenuItem( "Spec.", m_spectrum );
  m_upper_menu->addItem( item );
  
  // When outside the link area is clicked, the item doesnt get selected, so we'll work around this.
  item->clicked().connect( std::bind([this,item](){
    m_upper_menu->select( item );
    item->triggered().emit( item );
  }) );
  
  item = new WMenuItem( "Rel. Eff.", m_rel_eff_chart );
  m_upper_menu->addItem( item );
  
  item->clicked().connect( std::bind([this,item](){
    m_upper_menu->select( item );
    item->triggered().emit( item );
  }) );
  
  
  item = new WMenuItem( "Result", m_txt_results );
  m_upper_menu->addItem( item );
  
  item->clicked().connect( std::bind([this,item](){
    m_upper_menu->select( item );
    item->triggered().emit( item );
  }) );
  
  m_upper_menu->select( static_cast<int>(0) );
  
  m_interspec->spectrumScaleFactorChanged().connect( boost::bind(
                 &RelActAutoGui::handleDisplayedSpectrumChange, this, boost::placeholders::_1) );
  m_interspec->displayedSpectrumChanged().connect( boost::bind(
                 &RelActAutoGui::handleDisplayedSpectrumChange, this, boost::placeholders::_1) );
  
  m_default_par_sets_dir = SpecUtils::append_path( InterSpec::staticDataDirectory(), "rel_act" );
  const vector<string> default_par_sets = SpecUtils::recursive_ls( m_default_par_sets_dir, ".xml" );
  
  vector<string> user_par_sets;
#if( BUILD_AS_ELECTRON_APP || IOS || ANDROID || BUILD_AS_OSX_APP || BUILD_AS_LOCAL_SERVER )
  try
  {
    m_user_par_sets_dir = SpecUtils::append_path( InterSpec::writableDataDirectory(), "rel_act" );
    user_par_sets = SpecUtils::recursive_ls( m_user_par_sets_dir, ".xml" );
  }catch( std::exception & )
  {
    cerr << "RelActAutoGui: Writable data directory not set.\n";
  }
#endif
  
  WContainerWidget *presetDiv = new WContainerWidget( this );
  presetDiv->addStyleClass( "PresetsRow" );
  WLabel *label = new WLabel( "Option Presets", presetDiv );
  m_presets = new WComboBox( presetDiv );
  label->setBuddy( m_presets );
  
  m_presets->addItem( "Blank" );
  m_preset_paths.push_back( "" );
  
  for( const string &filename : default_par_sets )
  {
    string dispname = SpecUtils::filename(filename);
    if( dispname.size() > 4 )
      dispname = dispname.substr(0, dispname.size() - 4);
    
    m_presets->addItem( WString::fromUTF8(dispname) );
    m_preset_paths.push_back( filename );
  }//for( string filename : default_par_sets )
  
  for( const string &filename : user_par_sets )
  {
    string dispname = SpecUtils::filename(filename);
    if( dispname.size() > 4 )
      dispname = dispname.substr(0, filename.size() - 4);
    
    m_presets->addItem( WString::fromUTF8("User: " + dispname) );
    m_preset_paths.push_back( filename );
  }//for( string filename : default_par_sets )
  
  // m_presets->addItem( WString::fromUTF8("Custom") );
  
  m_presets->setCurrentIndex( 0 );
  m_current_preset_index = 0;
  m_presets->changed().connect( this, &RelActAutoGui::handlePresetChange );
  
  m_error_msg = new WText( "Not Calculated.", presetDiv );
  m_error_msg->addStyleClass( "RelActAutoErrMsg" );
  
  m_status_indicator = new WText( "Calculating...", presetDiv );
  m_status_indicator->addStyleClass( "RelActAutoStatusMsg" );
  m_status_indicator->hide();
  
  
  WContainerWidget *optionsDiv = new WContainerWidget( this );
  optionsDiv->addStyleClass( "RelActAutoOptions" );
  
  label = new WLabel( "Eqn Type", optionsDiv );
  label->addStyleClass( "GridFirstCol GridFirstRow" );
  
  m_rel_eff_eqn_form = new WComboBox( optionsDiv );
  m_rel_eff_eqn_form->addStyleClass( "GridSecondCol GridFirstRow" );
  label->setBuddy( m_rel_eff_eqn_form );
  m_rel_eff_eqn_form->activated().connect( this, &RelActAutoGui::handleRelEffEqnFormChanged );
  
  const char *tooltip = "The functional form to use for the relative efficiciency curve.<br />"
  "Options are:"
  "<table style=\"margin-left: 10px;\">"
  "<tr><th>Log(energy):</th>               <th>y = a + b*ln(x) + c*(ln(x))^2 + d*(ln(x))^3 + ...</th></tr>"
  "<tr><th>Log(rel. eff.):</th>            <th>y = exp( a + b*x + c/x + d/x^2 + e/x^3 + ... )</th></tr>"
  "<tr><th>Log(energy)Log(rel. eff.):</th> <th>y = exp( a  + b*(lnx) + c*(lnx)^2 + d*(lnx)^3 + ... )</th></tr>"
  "<tr><th>FRAM Empirical:</th>            <th>y = exp( a + b/x^2 + c*(lnx) + d*(lnx)^2 + e*(lnx)^3 )</th></tr>"
  "</table>";
  HelpSystem::attachToolTipOn( m_rel_eff_eqn_form, tooltip, showToolTips );
  HelpSystem::attachToolTipOn( label, tooltip, showToolTips );
  
  
  // Will assume FramEmpirical is the highest
  static_assert( static_cast<int>(RelActCalc::RelEffEqnForm::FramEmpirical)
                > static_cast<int>(RelActCalc::RelEffEqnForm::LnXLnY),
                "RelEffEqnForm was changed!"
                );
  
  
  for( int i = 0; i <= static_cast<int>(RelActCalc::RelEffEqnForm::FramEmpirical); ++i )
  {
    const auto eqn_form = RelActCalc::RelEffEqnForm( i );
    
    const char *txt = "";
    switch( eqn_form )
    {
      case RelActCalc::RelEffEqnForm::LnX:
        //y = a + b*ln(x) + c*(ln(x))^2 + d*(ln(x))^3 + ...
        txt = "Log(x)";
        break;
        
      case RelActCalc::RelEffEqnForm::LnY:
        //y = exp( a + b*x + c/x + d/x^2 + e/x^3 + ... )
        txt = "Log(y)";
        break;
        
      case RelActCalc::RelEffEqnForm::LnXLnY:
        //y = exp( a  + b*(lnx) + c*(lnx)^2 + d*(lnx)^3 + ... )
        txt = "Log(x)Log(y)";
        break;
        
      case RelActCalc::RelEffEqnForm::FramEmpirical:
        //y = exp( a + b/x^2 + c*(lnx) + d*(lnx)^2 + e*(lnx)^3 )
        txt = "Empirical";
        break;
    }
    
    m_rel_eff_eqn_form->addItem( txt );
  }//for( loop over RelEffEqnForm )
  
  m_rel_eff_eqn_form->setCurrentIndex( static_cast<int>(RelActCalc::RelEffEqnForm::LnX) );
  
  label = new WLabel( "Eqn Order", optionsDiv );
  label->addStyleClass( "GridThirdCol GridFirstRow" );
  
  m_rel_eff_eqn_order = new WComboBox( optionsDiv );
  m_rel_eff_eqn_order->addStyleClass( "GridFourthCol GridFirstRow" );
  label->setBuddy( m_rel_eff_eqn_order );
  m_rel_eff_eqn_order->activated().connect( this, &RelActAutoGui::handleRelEffEqnOrderChanged );
  
  m_rel_eff_eqn_order->addItem( "1" );
  m_rel_eff_eqn_order->addItem( "2" );
  m_rel_eff_eqn_order->addItem( "3" );
  m_rel_eff_eqn_order->addItem( "4" );
  m_rel_eff_eqn_order->addItem( "5" );
  m_rel_eff_eqn_order->addItem( "6" );
  m_rel_eff_eqn_order->setCurrentIndex( 2 );
  
  
  tooltip = "The order (how many energy-dependent terms) relative efficiency equation to use.";
  HelpSystem::attachToolTipOn( label, tooltip, showToolTips );
  HelpSystem::attachToolTipOn( m_rel_eff_eqn_order, tooltip, showToolTips );
  
  label = new WLabel( "FWHM Form", optionsDiv );
  label->addStyleClass( "GridFifthCol GridFirstRow" );
  
  m_fwhm_eqn_form = new WComboBox( optionsDiv );
  m_fwhm_eqn_form->addStyleClass( "GridSixthCol GridFirstRow" );
  label->setBuddy( m_fwhm_eqn_form );
  
  for( int i = 0; i <= static_cast<int>(RelActCalcAuto::FwhmForm::Polynomial_6); ++i )
  {
    const char *name = "";
    switch( RelActCalcAuto::FwhmForm(i) )
    {
      case RelActCalcAuto::FwhmForm::Gadras:       name = "Gadras"; break;
      case RelActCalcAuto::FwhmForm::SqrtEnergyPlusInverse:  name = "sqrt(A0 + A1*E + A2/E)"; break;
      case RelActCalcAuto::FwhmForm::Polynomial_2: name = "sqrt(A0 + A1*E)"; break;
      case RelActCalcAuto::FwhmForm::Polynomial_3: name = "sqrt(A0 + A1*E + A2*E*E)"; break;
      case RelActCalcAuto::FwhmForm::Polynomial_4: name = "sqrt(A0 + A1*E^1...A3*E^3)"; break;
      case RelActCalcAuto::FwhmForm::Polynomial_5: name = "sqrt(A0 + A1*E^1...A4*E^4)"; break;
      case RelActCalcAuto::FwhmForm::Polynomial_6: name = "sqrt(A0 + A1*E^1...A5*E^5)"; break;
    }//switch( RelActCalcAuto::FwhmForm(i) )
    
    m_fwhm_eqn_form->addItem( name );
  }//for( loop over RelActCalcAuto::FwhmForm )
  
  tooltip = "The equation type used to model peak FWHM as a function of energy.";
  HelpSystem::attachToolTipOn( label, tooltip, showToolTips );
  HelpSystem::attachToolTipOn( m_fwhm_eqn_form, tooltip, showToolTips );
  
  // TODO: need to set m_fwhm_eqn_form based on energy ranges selected
  m_fwhm_eqn_form->setCurrentIndex( 1 );
  m_fwhm_eqn_form->changed().connect( this, &RelActAutoGui::handleFwhmFormChanged );
  
/*
  label = new WLabel( "Yield Info", optionsDiv );
  label->addStyleClass( "GridSeventhCol GridFirstRow" );
  
  m_u_pu_data_source = new WComboBox( optionsDiv );
  label->setBuddy( m_u_pu_data_source );
  m_u_pu_data_source->activated().connect( this, &RelActAutoGui::handleNucDataSrcChanged );
  m_u_pu_data_source->addStyleClass( "GridEighthCol GridFirstRow" );
  
  tooltip = "The nuclear data source for gamma branching ratios of uranium and plutonium.";
  HelpSystem::attachToolTipOn( label, tooltip, showToolTips );
  HelpSystem::attachToolTipOn( m_u_pu_data_source, tooltip, showToolTips );
  
  using RelActCalcManual::PeakCsvInput::NucDataSrc;
  for( NucDataSrc src = NucDataSrc(0); src < NucDataSrc::Undefined; src = NucDataSrc(static_cast<int>(src) + 1) )
  {
    const char *src_label = "";
    switch( src )
    {
      case NucDataSrc::Icrp107_U:         src_label = "ICRP 107";   break;
      case NucDataSrc::Lanl_U:            src_label = "FRAM";       break;
      case NucDataSrc::IcrpLanlGadras_U:  src_label = "Combo";      break;
      case NucDataSrc::SandiaDecay:       src_label = "InterSpec";  break;
      case NucDataSrc::Undefined:         assert( 0 );              break;
    }//switch( src )
    
    m_u_pu_data_source->addItem( WString::fromUTF8(src_label) );
  }//for( loop over sources )
  
  m_u_pu_data_source->setCurrentIndex( static_cast<int>(NucDataSrc::SandiaDecay) );
  m_u_pu_data_source->changed().connect( this, &RelActAutoGui::handleDataSrcChanged );
 */
  
  m_fit_energy_cal = new WCheckBox( "Fit Energy Cal.", optionsDiv );
  m_fit_energy_cal->addStyleClass( "GridFirstCol GridSecondRow GridSpanTwoCol" );
  m_fit_energy_cal->checked().connect( this, &RelActAutoGui::handleFitEnergyCalChanged );
  m_fit_energy_cal->unChecked().connect( this, &RelActAutoGui::handleFitEnergyCalChanged );
  
  
  m_background_subtract = new WCheckBox( "Back. Sub.", optionsDiv );
  m_background_subtract->addStyleClass( "GridThirdCol GridSecondRow GridSpanTwoCol" );
  m_background_subtract->checked().connect( this, &RelActAutoGui::handleBackgroundSubtractChanged );
  m_background_subtract->unChecked().connect( this, &RelActAutoGui::handleBackgroundSubtractChanged );
  
  
  m_same_z_age = new WCheckBox( "Same El. Same Age", optionsDiv );
  m_same_z_age->addStyleClass( "GridFifthCol GridSecondRow GridSpanTwoCol" );
  m_same_z_age->checked().connect( this, &RelActAutoGui::handleSameAgeChanged );
  m_same_z_age->unChecked().connect( this, &RelActAutoGui::handleSameAgeChanged );
  
  m_u_pu_by_correlation = new WCheckBox( "Pu242/U236 by cor", optionsDiv );
  m_u_pu_by_correlation->addStyleClass( "GridSeventhCol GridSecondRow GridSpanTwoCol" );
  m_u_pu_by_correlation->checked().connect( this, &RelActAutoGui::handleUPuByCorrelationChanged );
  m_u_pu_by_correlation->unChecked().connect( this, &RelActAutoGui::handleUPuByCorrelationChanged );
  tooltip = "Pu-242 and U-236 are often not directly observable in gamma spectra.  However, to"
  " correct for these isotopes when calculating enrichment, the expected contributions of these"
  " isotopes can be inferred from the other isotopes.  Checking this box will enable this.";
  HelpSystem::attachToolTipOn( label, tooltip, showToolTips );
  HelpSystem::attachToolTipOn( m_u_pu_by_correlation, tooltip, showToolTips );
  
  
  WContainerWidget *bottomArea = new WContainerWidget( this );
  bottomArea->addStyleClass( "EnergiesAndNuclidesHolder" );
  
  WContainerWidget *nuclidesHolder = new WContainerWidget( bottomArea );
  nuclidesHolder->addStyleClass( "NuclidesHolder" );
  
  WContainerWidget *energiesHolder = new WContainerWidget( bottomArea );
  energiesHolder->addStyleClass( "EnergiesHolder" );
  
  m_free_peaks_container = new WContainerWidget( bottomArea );
  m_free_peaks_container->addStyleClass( "FreePeaksHolder" );
  m_free_peaks_container->hide();
  
  WText *nuc_header = new WText( "Nuclides", nuclidesHolder );
  nuc_header->addStyleClass( "EnergyNucHeader" );
  
  m_nuclides = new WContainerWidget( nuclidesHolder );
  m_nuclides->addStyleClass( "EnergyNucContent" );
  
  WContainerWidget *nuc_footer = new WContainerWidget( nuclidesHolder );
  nuc_footer->addStyleClass( "EnergyNucFooter" );
  
  WPushButton *add_nuc_icon = new WPushButton( nuc_footer );
  add_nuc_icon->setStyleClass( "AddEnergyRangeOrNuc Wt-icon" );
  add_nuc_icon->setIcon("InterSpec_resources/images/plus_min_black.svg");
  add_nuc_icon->clicked().connect( this, &RelActAutoGui::handleAddNuclide );
  tooltip = "Add a nuclide.";
  HelpSystem::attachToolTipOn( add_nuc_icon, tooltip, showToolTips );
  
  WText *energy_header = new WText( "Energy Ranges", energiesHolder );
  energy_header->addStyleClass( "EnergyNucHeader" );
  
  m_energy_ranges = new WContainerWidget( energiesHolder );
  m_energy_ranges->addStyleClass( "EnergyNucContent" );
  
  WContainerWidget *energies_footer = new WContainerWidget( energiesHolder );
  energies_footer->addStyleClass( "EnergyNucFooter" );
  
  WPushButton *add_energy_icon = new WPushButton( energies_footer );
  add_energy_icon->setStyleClass( "AddEnergyRangeOrNuc Wt-icon" );
  add_energy_icon->setIcon("InterSpec_resources/images/plus_min_black.svg");
  add_energy_icon->clicked().connect( this, &RelActAutoGui::handleAddEnergy );
  
  tooltip = "Add an energy range.";
  HelpSystem::attachToolTipOn( add_energy_icon, tooltip, showToolTips );
  
  m_show_free_peak = new WPushButton( "add free peaks", energies_footer );
  m_show_free_peak->addStyleClass( "ShowFreePeaks LightButton" );
  m_show_free_peak->clicked().connect( this, &RelActAutoGui::handleShowFreePeaks );
  
  
  WText *free_peaks_header = new WText( "Free Peaks", m_free_peaks_container );
  free_peaks_header->addStyleClass( "EnergyNucHeader" );
  m_free_peaks = new WContainerWidget( m_free_peaks_container );
  m_free_peaks->addStyleClass( "EnergyNucContent" );
  
  WContainerWidget *free_peaks_footer = new WContainerWidget( m_free_peaks_container );
  free_peaks_footer->addStyleClass( "EnergyNucFooter" );
  
  WPushButton *add_free_peak_icon = new WPushButton( free_peaks_footer );
  add_free_peak_icon->setStyleClass( "AddEnergyRangeOrNuc Wt-icon" );
  add_free_peak_icon->setIcon("InterSpec_resources/images/plus_min_black.svg");
  add_free_peak_icon->clicked().connect( boost::bind( &RelActAutoGui::handleAddFreePeak, this, 0.0, true, true ) );
  
  WPushButton *hide_free_peak = new WPushButton( "Close", free_peaks_footer );
  hide_free_peak->addStyleClass( "HideFreePeaks LightButton" );
  hide_free_peak->clicked().connect( this, &RelActAutoGui::handleHideFreePeaks );
  
  tooltip = "Remove all free peaks, and hide the free peaks input.";
  HelpSystem::attachToolTipOn( hide_free_peak, tooltip, showToolTips );
  
  
  tooltip = "&quot;Free peaks&quot; are peaks with a specific energy, that are not associated with any nuclide,"
  " and their amplitude is fit to the best value for the data.  The peaks may optionally be released from the"
  " functional FWHM constraint as well.<br />"
  "Free peaks are useful to handle peaks from reactions, or from unidentified nuclides.";
  HelpSystem::attachToolTipOn( m_free_peaks_container, tooltip, showToolTips );
  HelpSystem::attachToolTipOn( m_show_free_peak, tooltip, showToolTips );
  
  
  m_render_flags |= RenderActions::UpdateSpectra;
  m_render_flags |= RenderActions::UpdateCalculations;
}//RelActAutoGui constructor
  

void RelActAutoGui::render( Wt::WFlags<Wt::RenderFlag> flags )
{
  if( m_render_flags.testFlag(RenderActions::UpdateSpectra) )
  {
    updateDuringRenderForSpectrumChange();
    m_render_flags |= RenderActions::UpdateCalculations;
  }
  
  if( m_render_flags.testFlag(RenderActions::UpdateFreePeaks)
     || m_render_flags.testFlag(RenderActions::UpdateEnergyRanges)
     || m_render_flags.testFlag(RenderActions::UpdateFitEnergyCal) )
  {
    updateDuringRenderForFreePeakChange();
    m_render_flags |= RenderActions::UpdateCalculations;
  }
  
  if( m_render_flags.testFlag(RenderActions::UpdateNuclidesPresent) )
  {
    updateDuringRenderForNuclideChange();
    m_render_flags |= RenderActions::UpdateCalculations;
  }
  
  if( m_render_flags.testFlag(RenderActions::UpdateEnergyRanges) )
  {
    updateDuringRenderForEnergyRangeChange();
    m_render_flags |= RenderActions::UpdateCalculations;
  }
  
  if( m_render_flags.testFlag(RenderActions::ChartToDefaultRange) )
  {
    updateSpectrumToDefaultEnergyRange();
  }
  
  if( m_render_flags.testFlag(RenderActions::UpdateCalculations) )
  {
    startUpdatingCalculation();
  }
  
  m_render_flags = 0;
  m_loading_preset = false;
  
  WContainerWidget::render( flags );
}//void render( Wt::WFlags<Wt::RenderFlag> flags )


void RelActAutoGui::handleDisplayedSpectrumChange( SpecUtils::SpectrumType type )
{
  switch( type )
  {
    case SpecUtils::SpectrumType::Foreground:
    case SpecUtils::SpectrumType::Background:
      break;
      
    case SpecUtils::SpectrumType::SecondForeground:
      return;
  }//switch( type )
  
  const auto fore = m_interspec->displayedHistogram( SpecUtils::SpectrumType::Foreground );
  const auto back = m_interspec->displayedHistogram( SpecUtils::SpectrumType::Background );
  const double backsf = m_interspec->displayScaleFactor( SpecUtils::SpectrumType::Background );
  
  const bool enableBackSub = (back && fore);
  if( enableBackSub != m_background_subtract->isEnabled() )
    m_background_subtract->setEnabled( enableBackSub );
  
  if( (fore == m_foreground) && (back == m_background) && (!back || (backsf == m_background_sf)) )
    return;
  
  
  if( fore != m_foreground )
  {
    m_cached_drf.reset();
    m_cached_all_peaks.clear();
    
    m_render_flags |= RenderActions::ChartToDefaultRange;
  }//if( fore != m_foreground )
  
  
  m_render_flags |= RenderActions::UpdateSpectra;
  scheduleRender();
}//void handleDisplayedSpectrumChange( SpecUtils::SpectrumType )


void RelActAutoGui::checkIfInUserConfigOrCreateOne()
{
  if( m_loading_preset )
    return;
  
  const int index = m_presets->currentIndex();
  if( m_current_preset_index >= static_cast<int>(m_preset_paths.size()) )
  {
    // We are in a user-modified state, go ahead and return
    return;
  }
  
  string name;
  if( m_current_preset_index == 0 )
    name = "User Created";
  else
    name = "Modified " + m_presets->itemText(m_current_preset_index).toUTF8();
  
  m_presets->addItem( WString::fromUTF8(name) );
  
  m_current_preset_index = m_presets->count() - 1;
  m_presets->setCurrentIndex( m_current_preset_index );
}//void checkIfInUserConfigOrCreateOne()


RelActCalcAuto::Options RelActAutoGui::getCalcOptions() const
{
  RelActCalcAuto::Options options;
  
  options.fit_energy_cal = m_fit_energy_cal->isChecked();
  options.nucs_of_el_same_age = m_same_z_age->isChecked();
  options.rel_eff_eqn_type = RelActCalc::RelEffEqnForm( std::max( 0, m_rel_eff_eqn_form->currentIndex() ) );
  options.rel_eff_eqn_order = 1 + std::max( 0, m_rel_eff_eqn_order->currentIndex() );
  options.fwhm_form = RelActCalcAuto::FwhmForm( std::max(0,m_fwhm_eqn_form->currentIndex()) );
  
  const shared_ptr<const SpecUtils::Measurement> fore
                           = m_interspec->displayedHistogram( SpecUtils::SpectrumType::Foreground );
  const shared_ptr<const SpecMeas> meas
                                   = m_interspec->measurment( SpecUtils::SpectrumType::Foreground );
  if( fore && !fore->title().empty() )
    options.spectrum_title = fore->title();
  else if( meas && !meas->filename().empty() )
    options.spectrum_title = meas->filename();
  
  return options;
}//RelActCalcAuto::Options getCalcOptions() const


vector<RelActCalcAuto::NucInputInfo> RelActAutoGui::getNucInputInfo() const
{
  vector<RelActCalcAuto::NucInputInfo> answer;
  
  const vector<WWidget *> &kids = m_nuclides->children();
  for( WWidget *w : kids )
  {
    const RelActAutoNuclide *nuc = dynamic_cast<const RelActAutoNuclide *>( w );
    assert( nuc );
    if( nuc && nuc->nuclide() )
      answer.push_back( nuc->toNucInputInfo() );
  }//for( WWidget *w : kids )
  
  return answer;
}//RelActCalcAuto::NucInputInfo getNucInputInfo() const


vector<RelActCalcAuto::RoiRange> RelActAutoGui::getRoiRanges() const
{
  vector<RelActCalcAuto::RoiRange> answer;
  const vector<WWidget *> &kids = m_energy_ranges->children();
  for( WWidget *w : kids )
  {
    const RelActAutoEnergyRange *roi = dynamic_cast<const RelActAutoEnergyRange *>( w );
    assert( roi );
    if( roi && !roi->isEmpty() )
      answer.push_back( roi->toRoiRange() );
  }//for( WWidget *w : kids )
  
  return answer;
}//RelActCalcAuto::RoiRange getRoiRanges() const


vector<RelActCalcAuto::FloatingPeak> RelActAutoGui::getFloatingPeaks() const
{
  // We will only return peaks within defined ROIs.
  vector<pair<float,float>> rois;
  const vector<WWidget *> &roi_widgets = m_energy_ranges->children();
  for( WWidget *w : roi_widgets )
  {
    const RelActAutoEnergyRange *roi = dynamic_cast<const RelActAutoEnergyRange *>( w );
    assert( roi );
    if( roi && !roi->isEmpty() )
      rois.emplace_back( roi->lowerEnergy(), roi->upperEnergy() );
  }//for( WWidget *w : kids )
  
  
  vector<RelActCalcAuto::FloatingPeak> answer;
  const std::vector<WWidget *> &kids = m_free_peaks->children();
  for( WWidget *w : kids )
  {
    RelActFreePeak *free_peak = dynamic_cast<RelActFreePeak *>(w);
    assert( free_peak );
    if( !free_peak )
      continue;
    
    const float energy = free_peak->energy();
    bool in_roi = false;
    for( const auto &roi : rois )
      in_roi = (in_roi || ((energy >= roi.first) && (energy <= roi.second)));
    
    if( !in_roi )
      continue;
    
    RelActCalcAuto::FloatingPeak peak;
    peak.energy = energy;
    peak.release_fwhm = !free_peak->fwhmConstrained();
    peak.apply_energy_cal_correction = free_peak->applyEnergyCal();
    
    answer.push_back( peak );
  }//for( loop over RelActFreePeak widgets )
  
  return answer;
}//RelActCalcAuto::FloatingPeak getFloatingPeaks() const


void RelActAutoGui::handleRoiDrag( double new_roi_lower_energy,
                   double new_roi_upper_energy,
                   double new_roi_lower_px,
                   double new_roi_upper_px,
                   const double original_roi_lower_energy,
                   const bool is_final_range )
{
  //cout << "RelActAutoGui::handleRoiDrag: original_roi_lower_energy=" << original_roi_lower_energy
  //<< ", new_roi_lower_energy=" << new_roi_lower_energy << ", new_roi_upper_energy=" << new_roi_upper_energy
  //<< ", is_final_range=" << is_final_range << endl;
  
  double min_de = 999999.9;
  RelActAutoEnergyRange *range = nullptr;
  
  const vector<WWidget *> &kids = m_energy_ranges->children();
  for( WWidget *w : kids )
  {
    RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>( w );
    assert( roi );
    if( !roi || roi->isEmpty() )
      continue;
    
    RelActCalcAuto::RoiRange roi_range = roi->toRoiRange();
    
    const double de = fabs(roi_range.lower_energy - original_roi_lower_energy);
    if( de < min_de )
    {
      min_de = de;
      range = roi;
    }
  }//for( WWidget *w : kids )

  if( !range || min_de > 1.0 )  //0.001 would probably be fine instead of 1.0
  {
    cerr << "Unexpectedly couldnt find ROI in getRoiRanges()!" << endl;
    return;
  }//if( failed to find continuum )
  
  if( is_final_range && (new_roi_upper_px < new_roi_lower_px) )
  {
    handleRemoveEnergy( range );
    return;
  }//if( the user
  
  // We will only set RelActAutoEnergyRange energies when its final, otherwise the lower energy wont
  //  match on later updates
  if( is_final_range )
  {
    range->setEnergyRange( new_roi_lower_energy, new_roi_upper_energy );
    
    handleEnergyRangeChange();
  }else
  {
    shared_ptr<const deque<PeakModel::PeakShrdPtr>> origpeaks = m_peak_model->peaks();
    if( !origpeaks )
      return;
    
    double minDe = 999999.9;
    std::shared_ptr<const PeakContinuum> continuum;
    for( auto p : *origpeaks )
    {
      const double de = fabs( p->continuum()->lowerEnergy() - original_roi_lower_energy );
      if( de < min_de )
      {
        min_de = de;
        continuum = p->continuum();
      }
    }//for( auto p : *origpeaks )
    
    if( !continuum || min_de > 1.0 )  //0.001 would probably be fine instead of 1.0
    {
      m_spectrum->updateRoiBeingDragged( {} );
      return;
    }//if( failed to find continuum )
    
    
    auto new_continuum = std::make_shared<PeakContinuum>( *continuum );
    
    // re-use the c++ ROI value that we arent dragging, to avoid rounding or whatever
    const bool dragginUpperEnergy = (fabs(new_roi_lower_energy - continuum->lowerEnergy())
                                      < fabs(new_roi_upper_energy - continuum->upperEnergy()));
    
    if( dragginUpperEnergy )
      new_roi_lower_energy = continuum->lowerEnergy();
    else
      new_roi_upper_energy = continuum->upperEnergy();
    
    new_continuum->setRange( new_roi_lower_energy, new_roi_upper_energy );
    
    vector< shared_ptr<const PeakDef>> new_roi_initial_peaks;
    for( auto p : *origpeaks )
    {
      if( p->continuum() == continuum )
      {
        auto newpeak = make_shared<PeakDef>(*p);
        newpeak->setContinuum( new_continuum );
        new_roi_initial_peaks.push_back( newpeak );
      }
    }//for( auto p : *origpeaks )
    
    m_spectrum->updateRoiBeingDragged( new_roi_initial_peaks );
  }//if( is_final_range )
}//void handleRoiDrag(...)


void RelActAutoGui::handleCreateRoiDrag( const double lower_energy,
                         const double upper_energy,
                         const int num_peaks_to_force,
                         const bool is_final_range )
{
  //cout << "RelActAutoGui::handleCreateRoiDrag: lower_energy=" << lower_energy
  //<< ", upper_energy=" << upper_energy << ", num_peaks_to_force=" << num_peaks_to_force
  //<< ", is_final_range=" << is_final_range << endl;
  
  if( !m_foreground )
  {
    m_spectrum->updateRoiBeingDragged( {} );
    return;
  }
  
  const double roi_lower = std::min( lower_energy, upper_energy );
  const double roi_upper = std::max( lower_energy, upper_energy );
  
  if( is_final_range )
  {
    m_spectrum->updateRoiBeingDragged( {} );
    
    RelActAutoEnergyRange *energy_range = new RelActAutoEnergyRange( this, m_energy_ranges );
    
    energy_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    
    energy_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                                this, static_cast<WWidget *>(energy_range) ) );
    
    energy_range->setEnergyRange( roi_lower, roi_upper );
    energy_range->setForceFullRange( true );
    
    handleEnergyRangeChange();
    
    return;
  }//if( is_final_range )
  
  
  try
  {
    // Make a single peak with zero amplitude to at least give feedback (the continuum range line)
    //  to the user about where they have dragged.
    const double mean = 0.5*(roi_lower + roi_upper);
    const double sigma = 0.5*fabs( roi_upper - roi_lower );
    const double amplitude = 0.0;
    
    auto peak = make_shared<PeakDef>(mean, sigma, amplitude );
    
    peak->continuum()->setRange( roi_lower, roi_upper );
    peak->continuum()->calc_linear_continuum_eqn( m_foreground, mean, roi_lower, roi_upper, 3, 3 );
    
    m_spectrum->updateRoiBeingDragged( vector< shared_ptr<const PeakDef>>{peak} );
  }catch( std::exception &e )
  {
    m_spectrum->updateRoiBeingDragged( {} );
    cerr << "RelActAutoGui::handleCreateRoiDrag caught exception: " << e.what() << endl;
    return;
  }//try / catch
}//void handleCreateRoiDrag(...)


void RelActAutoGui::handleShiftDrag( const double lower_energy, const double upper_energy )
{
  const vector<WWidget *> kids = m_energy_ranges->children();
  for( WWidget *w : kids )
  {
    RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>( w );
    assert( roi );
    if( !roi || roi->isEmpty() )
      continue;
    
    const double roi_lower = roi->lowerEnergy();
    const double roi_upper = roi->upperEnergy();
    
    // If the ranges intersect, deal with it
    if( (upper_energy >= roi_lower) && (lower_energy <= roi_upper) )
      handleRemovePartOfEnergyRange( w, lower_energy, upper_energy );
  }//for( WWidget *w : kids )
}//void handleShiftDrag( const double lower_energy, const double upper_energy )


void RelActAutoGui::handleDoubleLeftClick( const double energy, const double /* counts */ )
{
  try
  {
    char buffer[256] = { '\0' };
    
    // Check if click was in a ROI, and if so ignore it
    const vector<RelActCalcAuto::RoiRange> orig_rois = getRoiRanges();
    for( const RelActCalcAuto::RoiRange &roi : orig_rois )
    {
      if( (energy > roi.lower_energy) && (energy < roi.upper_energy) )
      {
        snprintf( buffer, sizeof(buffer),
                 "%.1f keV is already in the energy range [%.1f, %.1f] keV - no action will be taken.",
                 energy, roi.lower_energy, roi.upper_energy );
        
        passMessage( buffer, "", WarningWidget::WarningMsgMedium );
        return;
      }
    }//for( const RelActCalcAuto::RoiRange &roi : orig_rois )
    
    // If we're here, the double-click was not in an existing ROI.
    if( !m_foreground )
      return;
    
    const double xmin = m_spectrum->xAxisMinimum();
    const double xmax = m_spectrum->xAxisMaximum();
    
    const double specWidthPx = m_spectrum->chartWidthInPixels();
    const double pixPerKeV = (xmax > xmin && xmax > 0.0 && specWidthPx > 10.0) ? std::max(0.001,(specWidthPx/(xmax - xmin))): 0.001;
    
    // We'll prefer the DRF from m_solution, to what the user has picked
    shared_ptr<const DetectorPeakResponse> det = m_solution ? m_solution->m_drf : nullptr;
    
    if( !det || !det->hasResolutionInfo()  )
    {
      shared_ptr<const SpecMeas> meas = m_interspec->measurment(SpecUtils::SpectrumType::Foreground);
      det = meas ? meas->detector() : nullptr;
    }//if( solution didnt have DRF )
    
    const auto found_peaks = searchForPeakFromUser( energy, pixPerKeV, m_foreground, {}, det );
    
    // If we didnt fit a peak, and we dont
    double lower_energy = energy - 10;
    double upper_energy = energy + 10;
    shared_ptr<const PeakDef> peak = found_peaks.first.empty() ? nullptr : found_peaks.first.front();
    if( peak )
    {
      // Found a peak
      lower_energy = peak->lowerX();
      upper_energy = peak->upperX();
    }else if( det && det->hasResolutionInfo() )
    {
      // No peak found, but we have a DRF with FWHM info
      const float fwhm = std::max( 1.0f, det->peakResolutionFWHM(energy) );
      
      // We'll make a ROI that is +-4 FWHM (arbitrary choice)
      lower_energy = energy - 4*fwhm;
      upper_energy = energy + 4*fwhm;
    }else
    {
      // If we're here, were desperate - lets pluck some values out of the air
      //  (We could check on auto-searched peaks and estimate from those, but
      //   really, at this point, its not worth the effort as things are probably
      //   low quality)
      const bool highres = PeakFitUtils::is_high_res(m_foreground);
      if( highres )
      {
        lower_energy = energy - 5;
        upper_energy = energy + 5;
      }else
      {
        lower_energy = 0.8*energy;
        upper_energy = 1.2*energy;
      }
    }//if( peak ) / else if( det ) / else
    
    RelActCalcAuto::RoiRange new_roi;
    new_roi.lower_energy = lower_energy;
    new_roi.upper_energy = upper_energy;
    new_roi.continuum_type = PeakContinuum::OffsetType::Linear;
    new_roi.force_full_range = true;
    new_roi.allow_expand_for_peak_width = false;
    
    
    // Now check if we overlap with a ROI, or perhaps we need to expand an existing ROI
    
    const vector<WWidget *> prev_roi_widgets = m_energy_ranges->children();
    
    RelActAutoEnergyRange *new_roi_w = new RelActAutoEnergyRange( this, m_energy_ranges );
    new_roi_w->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    new_roi_w->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy, this, static_cast<WWidget *>(new_roi_w) ) );
    new_roi_w->setFromRoiRange( new_roi );
    
    vector<RelActAutoEnergyRange *> overlapping_rois;
    for( WWidget *w : prev_roi_widgets )
    {
      RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>( w );
      assert( roi );
      if( !roi )
        continue;
      if( (new_roi.upper_energy > roi->lowerEnergy()) && (new_roi.lower_energy < roi->upperEnergy()) )
        overlapping_rois.push_back( roi );
    }
    
    bool combined_an_roi = false;
    for( RelActAutoEnergyRange *roi : overlapping_rois )
    {
      WWidget *w = handleCombineRoi( roi, new_roi_w );
      RelActAutoEnergyRange *combined_roi = dynamic_cast<RelActAutoEnergyRange *>( w );
      
      assert( combined_roi );
      if( !combined_roi )
        break;
      
      combined_an_roi = true;
      new_roi_w = combined_roi;
    }
    
    lower_energy = new_roi_w->lowerEnergy();
    upper_energy = new_roi_w->upperEnergy();
    
    if( combined_an_roi )
    {
      snprintf( buffer, sizeof(buffer),
               "Extended existing energy range to [%.1f, %.1f] keV.",
               lower_energy, upper_energy );
    }else
    {
      snprintf( buffer, sizeof(buffer),
               "Added a new new energy range from %.1f to %.1f keV.",
               lower_energy, upper_energy );
    }//if( combined_an_roi ) / else
        
    passMessage( buffer, "", WarningWidget::WarningMsgLow );
    
    checkIfInUserConfigOrCreateOne();
    m_render_flags |= RenderActions::UpdateEnergyRanges;
    m_render_flags |= RenderActions::UpdateCalculations;
    scheduleRender();
  }catch( std::exception &e )
  {
    passMessage( "handleDoubleLeftClick error: " + string(e.what()), "", WarningWidget::WarningMsgHigh );
  }//try / catch
}//void handleDoubleLeftClick( const double energy, const double counts )


void RelActAutoGui::handleRightClick( const double energy, const double counts,
                      const int page_x_px, const int page_y_px )
{
  vector<RelActAutoEnergyRange *> ranges;
  const vector<WWidget *> &kids = m_energy_ranges->children();
  for( WWidget *w : kids )
  {
    RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>( w );
    assert( roi );
    if( roi && !roi->isEmpty() )
       ranges.push_back( roi );
  }//for( WWidget *w : kids )
  
  std::sort( begin(ranges), end(ranges),
             []( const RelActAutoEnergyRange *lhs, const RelActAutoEnergyRange *rhs) -> bool{
    return lhs->lowerEnergy() < rhs->lowerEnergy();
  } );
  
  
  RelActAutoEnergyRange *range = nullptr, *range_to_left = nullptr, *range_to_right = nullptr;
  for( size_t i = 0; i < ranges.size(); ++i )
  {
    RelActAutoEnergyRange *roi = ranges[i];
    RelActCalcAuto::RoiRange roi_range = roi->toRoiRange();
  
    if( (energy >= roi_range.lower_energy) && (energy < roi_range.upper_energy) )
    {
      range = roi;
      if( i > 0 )
        range_to_left = ranges[i-1];
      if( (i + 1) < ranges.size() )
        range_to_right = ranges[i+1];
      
      break;
    }//
  }//for( RelActAutoEnergyRange *roi : ranges )
  
  if( !range )
  {
    cerr << "Right-click is not in an ROI" << endl;
    return;
  }//if( failed to find continuum )
  
  
  DeleteOnClosePopupMenu *menu = new DeleteOnClosePopupMenu( nullptr, PopupDivMenu::TransientMenu );
  menu->aboutToHide().connect( menu, &DeleteOnClosePopupMenu::markForDelete );
  menu->setPositionScheme( Wt::Absolute );
  
  PopupDivMenuItem *item = nullptr;
  
  const bool is_phone = false; //isPhone();
  if( is_phone )
    item = menu->addPhoneBackItem( nullptr );
  
  item = menu->addMenuItem( "ROI options:" );
  item->disable();
  item->setSelectable( false );
  menu->addSeparator();
  
  const auto roi = range->toRoiRange();
  
  PopupDivMenu *continuum_menu = menu->addPopupMenuItem( "Set Continuum Type" );
  for( auto type = PeakContinuum::OffsetType(0);
      type < PeakContinuum::External; type = PeakContinuum::OffsetType(type+1) )
  {
    WMenuItem *item = continuum_menu->addItem( PeakContinuum::offset_type_label(type) );
    item->triggered().connect( boost::bind( &RelActAutoEnergyRange::setContinuumType, range, type ) );
    if( type == roi.continuum_type )
      item->setDisabled( true );
  }//for( loop over PeakContinuum::OffsetTypes )
  
  item = menu->addMenuItem( "Remove ROI" );
  item->triggered().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy, this, static_cast<WWidget *>(range) ) );
  
  char buffer[128] = { '\0' };
  snprintf( buffer, sizeof(buffer), "Split ROI at %.1f keV", energy );
  item = menu->addMenuItem( buffer );
  item->triggered().connect( boost::bind( &RelActAutoGui::handleSplitEnergyRange, this, static_cast<WWidget *>(range), energy ) );
  
  const char *item_label = "";
  if( roi.force_full_range )
    item_label = "Don't force full-range";
  else
    item_label = "Force full-range";
  item = menu->addMenuItem( item_label );
  item->triggered().connect( boost::bind( &RelActAutoGui::handleToggleForceFullRange, this, static_cast<WWidget *>(range) ) );
  
  // TODO: we could be a little more intelligent about when offering to combine ROIs
  if( range_to_left )
  {
    item = menu->addMenuItem( "Combine with ROI to left" );
    item->triggered().connect( boost::bind( &RelActAutoGui::handleCombineRoi, this,
                                           static_cast<WWidget *>(range_to_left),
                                           static_cast<WWidget *>(range) ) );
  }//if( range_to_left )
  
  if( range_to_right )
  {
    item = menu->addMenuItem( "Combine with ROI to right" );
    item->triggered().connect( boost::bind( &RelActAutoGui::handleCombineRoi, this,
                                           static_cast<WWidget *>(range),
                                           static_cast<WWidget *>(range_to_right) ) );
  }//if( range_to_right )
  
  // TODO: Add floating peak item
  snprintf( buffer, sizeof(buffer), "Add free peak at %.1f keV", energy );
  item = menu->addMenuItem( buffer );
  item->triggered().connect( boost::bind( &RelActAutoGui::handleAddFreePeak, this, energy, true, true ) );
  
  
  if( is_phone )
  {
    menu->addStyleClass( " Wt-popupmenu Wt-outset" );
    menu->showMobile();
  }else
  {
    menu->addStyleClass( " Wt-popupmenu Wt-outset RelActAutoGuiContextMenu" );
    menu->popup( WPoint(page_x_px - 30, page_y_px - 30) );
  }
}//void handleRightClick(...)


void RelActAutoGui::setCalcOptionsGui( const RelActCalcAuto::Options &options )
{
  m_fit_energy_cal->setChecked( options.fit_energy_cal );
  m_same_z_age->setChecked( options.nucs_of_el_same_age );
  m_rel_eff_eqn_form->setCurrentIndex( static_cast<int>(options.rel_eff_eqn_type) );
  m_rel_eff_eqn_order->setCurrentIndex( std::max(options.rel_eff_eqn_order,size_t(1)) - 1 );
  m_fwhm_eqn_form->setCurrentIndex( static_cast<int>(options.fwhm_form) );
  
  // options.spectrum_title
  
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void setCalcOptionsGui( const RelActCalcAuto::Options &options )


void RelActAutoGui::handleToggleForceFullRange( Wt::WWidget *w )
{
  if( !w )
    return;
  
  const std::vector<WWidget *> &kids = m_energy_ranges->children();
  const auto pos = std::find( begin(kids), end(kids), w );
  if( pos == end(kids) )
  {
    cerr << "Failed to find a RelActAutoEnergyRange in m_energy_ranges!" << endl;
    assert( 0 );
    return;
  }
  
  assert( dynamic_cast<RelActAutoEnergyRange *>(w) );
  
  RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>(w);
  assert( roi );
  if( !roi )
    return;
  
  roi->setForceFullRange( !roi->forceFullRange() );
}//void handleToggleForceFullRange( Wt::WWidget *w )


Wt::WWidget *RelActAutoGui::handleCombineRoi( Wt::WWidget *left_roi, Wt::WWidget *right_roi )
{
  if( !left_roi || !right_roi || (left_roi == right_roi) )
  {
    assert( 0 );
    return nullptr;
  }
  
  const std::vector<WWidget *> &kids = m_energy_ranges->children();
  const auto left_pos = std::find( begin(kids), end(kids), left_roi );
  const auto right_pos = std::find( begin(kids), end(kids), right_roi );
  if( (left_pos == end(kids)) || (right_pos == end(kids)) )
  {
    cerr << "Failed to find left or right RelActAutoEnergyRange in m_energy_ranges!" << endl;
    assert( 0 );
    return nullptr;
  }
  
  RelActAutoEnergyRange *left_range = dynamic_cast<RelActAutoEnergyRange *>(left_roi);
  RelActAutoEnergyRange *right_range = dynamic_cast<RelActAutoEnergyRange *>(right_roi);
  
  assert( left_range && right_range );
  if( !left_range || !right_range )
    return nullptr;
  
  const RelActCalcAuto::RoiRange lroi = left_range->toRoiRange();
  const RelActCalcAuto::RoiRange rroi = right_range->toRoiRange();
  
  RelActCalcAuto::RoiRange new_roi = lroi;
  new_roi.lower_energy = std::min( lroi.lower_energy, rroi.lower_energy );
  new_roi.upper_energy = std::max( lroi.upper_energy, rroi.upper_energy );
  if( rroi.force_full_range )
    new_roi.force_full_range = true;
  if( rroi.allow_expand_for_peak_width )
    new_roi.allow_expand_for_peak_width = true;
  new_roi.continuum_type = std::max( lroi.continuum_type, rroi.continuum_type );
  
  delete right_range;
  right_roi = nullptr;
  right_range = nullptr;
  
  left_range->setFromRoiRange( new_roi );
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
  
  return left_range;
}//void handleCombineRoi( Wt::WWidget *left_roi, Wt::WWidget *right_roi );


rapidxml::xml_node<char> *RelActAutoGui::serialize( rapidxml::xml_node<char> *parent_node ) const
{
  assert( parent_node );
  
  using namespace rapidxml;
  xml_document<char> * const doc = parent_node->document();
  
  assert( doc );
  if( !doc )
    throw runtime_error( "RelActAutoGui::serialize: no xml_document" );
  
  xml_node<char> *base_node = doc->allocate_node( node_element, "RelActCalcAuto" );
  parent_node->append_node( base_node );
  
  xml_attribute<char> *attrib = doc->allocate_attribute( "version", "0" );
  base_node->append_attribute( attrib );
  
  // Elements in the offline-xml we dont deal with here
  //  base_node->append_node( <"ForegroundFileName"> );
  //  base_node->append_node( <"BackgroundFileName"> );
  //  base_node->append_node( <"OutputHtmlFileName"> );
  //  base_node->append_node( <"RoiAndNucsFromFile"> );
  
  const RelActCalcAuto::Options options = getCalcOptions();
  const vector<RelActCalcAuto::RoiRange> rois = getRoiRanges();
  const vector<RelActCalcAuto::NucInputInfo> nuclides = getNucInputInfo();
  const vector<RelActCalcAuto::FloatingPeak> floating_peaks = getFloatingPeaks();
  rapidxml::xml_node<char> *options_node = options.toXml( base_node );
  
  {// Begin put extra options
    //options_node
    if( m_u_pu_by_correlation->isVisible() )
    {
      const char *val = m_u_pu_by_correlation->isChecked() ? "true" : "false";
      xml_node<char> *node = doc->allocate_node( node_element, "UPuByCorrelation", val );
      options_node->append_node( node );
    }//if( we have U or Pu in the problem )
    
    /*
    if( m_u_pu_data_source->isVisible() )
    {
      const int src_index = m_u_pu_data_source->currentIndex();
      if( (src_index>= 0)
         && (src_index < static_cast<int>(RelActCalcManual::PeakCsvInput::NucDataSrc::Undefined)) )
      {
        const auto src_type = RelActCalcManual::PeakCsvInput::NucDataSrc( src_index );
        const char *val = RelActCalcManual::PeakCsvInput::to_str( src_type );
        xml_node<char> *node = doc->allocate_node( node_element, "UPuDataSrc", val );
        options_node->append_node( node );
      }//if( a valid index )
    }//if( we hav eU or Pu in the problem )
     */
  
  
    if( m_background_subtract->isEnabled() )
    {
      const char *val = m_background_subtract->isChecked() ? "true" : "false";
      xml_node<char> *node = doc->allocate_node( node_element, "BackgroundSubtract", val );
      options_node->append_node( node );
    }//if( we have a background to subtract )
  }// End put extra options
  
  
  if( !rois.empty() )
  {
    xml_node<char> *node = doc->allocate_node( node_element, "RoiRangeList" );
    base_node->append_node( node );
    for( const auto &range : rois )
      range.toXml( node );
  }
  
  if( !nuclides.empty() )
  {
    xml_node<char> *node = doc->allocate_node( node_element, "NucInputInfoList" );
    base_node->append_node( node );
    for( const auto &nuc : nuclides )
      nuc.toXml( node );
  }//if( !nuclides.empty() )
  
  if( !floating_peaks.empty() )
  {
    xml_node<char> *node = doc->allocate_node( node_element, "FloatingPeakList" );
    base_node->append_node( node );
    for( const auto &peak : floating_peaks )
      peak.toXml( node );
  }//if( !floating_peaks.empty() )
  
  
  // TODO: put in display options, such as spectrum energy range, log/lin (also, make our spectrum obey this pref value),
  
  
  return parent_node;
}//rapidxml::xml_node<char> *RelActAutoGui::serialize( rapidxml::xml_node<char> *parent )


void RelActAutoGui::deSerialize( const rapidxml::xml_node<char> *base_node )
{
  using namespace rapidxml;
  
  assert( base_node );
  if( !base_node )
    throw runtime_error( "RelActAutoGui::deSerialize: nullptr passed in." );
  
  const string base_node_name = SpecUtils::xml_name_str(base_node);
  if( base_node_name != "RelActCalcAuto" )
    throw runtime_error( "RelActAutoGui::deSerialize: invalid node passed in named '"
                         + base_node_name + "'" );
  
  const xml_attribute<char> *attrib = XML_FIRST_ATTRIB(base_node, "version");
  const string base_version = SpecUtils::xml_value_str(attrib);
  if( !SpecUtils::istarts_with(base_version, "0") )
    throw runtime_error( "RelActAutoGui::deSerialize: invalid xml version='" + base_version + "'" );
  
  const xml_node<char> *node = XML_FIRST_NODE(base_node, "Options");
  if( !node )
    throw runtime_error( "RelActAutoGui::deSerialize: No <Options> node." );
  
  RelActCalcAuto::Options options;
  vector<RelActCalcAuto::RoiRange> rois;
  vector<RelActCalcAuto::NucInputInfo> nuclides;
  vector<RelActCalcAuto::FloatingPeak> floating_peaks;
  
  options.fromXml( node );
  
  
  {// Begin get extra options
    const xml_node<char> *opt_node = XML_FIRST_NODE(node, "UPuByCorrelation");
    string val = SpecUtils::xml_value_str( opt_node );
    const bool by_correlation = !SpecUtils::iequals_ascii(val, "false");
    m_u_pu_by_correlation->setChecked(by_correlation);
    
    opt_node = XML_FIRST_NODE(node, "BackgroundSubtract");
    val = SpecUtils::xml_value_str( opt_node );
    const bool back_sub = SpecUtils::iequals_ascii(val, "true");
    
    /*
    opt_node = XML_FIRST_NODE(node, "UPuDataSrc");
    val = SpecUtils::xml_value_str( opt_node );
    if( !val.empty() )
    {
      using RelActCalcManual::PeakCsvInput::NucDataSrc;
      
      bool set_src = false;
      for( int i = 0; i < static_cast<int>(NucDataSrc::Undefined); ++i )
      {
        const NucDataSrc src = NucDataSrc(i);
        const char *src_str = RelActCalcManual::PeakCsvInput::to_str(src);
        if( val == src_str )
        {
          set_src = true;
          m_u_pu_data_source->setCurrentIndex( i );
          break;
        }
      }//for( int i = 0; i < static_cast<int>(NucDataSrc::Undefined); ++i )
      
      if( !set_src )
        cerr << "Failed to convert '" << val << "' into a NucDataSrc" << endl;
    }//if( UPuDataSrc not empty )
     */
  }// End get extra options
  

  node = XML_FIRST_NODE(base_node, "RoiRangeList");
  if( node )
  {
    XML_FOREACH_DAUGHTER( roi_node, node, "RoiRange" )
    {
      RelActCalcAuto::RoiRange roi;
      roi.fromXml( roi_node );
      rois.push_back( roi );
    }
  }//if( <RoiRangeList> )
  
  
  node = XML_FIRST_NODE(base_node, "NucInputInfoList");
  if( node )
  {
    XML_FOREACH_DAUGHTER( nuc_node, node, "NucInputInfo" )
    {
      RelActCalcAuto::NucInputInfo nuc;
      nuc.fromXml( nuc_node );
      nuclides.push_back( nuc );
    }
  }//if( <NucInputInfoList> )
  
  
  node = XML_FIRST_NODE(base_node, "FloatingPeakList");
  if( node )
  {
    XML_FOREACH_DAUGHTER( peak_node, node, "FloatingPeak" )
    {
      RelActCalcAuto::FloatingPeak peak;
      peak.fromXml( peak_node );
      floating_peaks.push_back( peak );
    }
  }//if( <NucInputInfoList> )
  
  m_loading_preset = true;
  
  setCalcOptionsGui( options );
  m_nuclides->clear();
  m_energy_ranges->clear();
  
  for( const RelActCalcAuto::NucInputInfo &nuc : nuclides )
  {
    RelActAutoNuclide *nuc_widget = new RelActAutoNuclide( this, m_nuclides );
    nuc_widget->updated().connect( this, &RelActAutoGui::handleNuclidesChanged );
    nuc_widget->remove().connect( boost::bind( &RelActAutoGui::handleRemoveNuclide,
                                              this, static_cast<WWidget *>(nuc_widget) ) );
    nuc_widget->fromNucInputInfo( nuc );
  }//for( const RelActCalcAuto::NucInputInfo &nuc : nuclides )
  
  
  for( const RelActCalcAuto::RoiRange &roi : rois )
  {
    RelActAutoEnergyRange *energy_range = new RelActAutoEnergyRange( this, m_energy_ranges );
    
    energy_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    
    energy_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                                this, static_cast<WWidget *>(energy_range) ) );
    
    energy_range->setFromRoiRange( roi );
  }//for( const RelActCalcAuto::RoiRange &roi : rois )
  
  
  // Free Peaks
  m_free_peaks->clear();
  if( floating_peaks.empty() && !m_free_peaks_container->isHidden() )
    handleHideFreePeaks();
  
  if( !floating_peaks.empty() && m_free_peaks_container->isHidden() )
    handleShowFreePeaks();
  
  for( const RelActCalcAuto::FloatingPeak &peak : floating_peaks )
    handleAddFreePeak( peak.energy, !peak.release_fwhm, peak.apply_energy_cal_correction );
  
  m_solution.reset();
  m_peak_model->setPeaks( vector<PeakDef>{} );
  
  m_render_flags |= RenderActions::UpdateNuclidesPresent;
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  m_render_flags |= RenderActions::UpdateFreePeaks;
  scheduleRender();
}//void deSerialize( const rapidxml::xml_node<char> *base_node )


std::unique_ptr<rapidxml::xml_document<char>> RelActAutoGui::guiStateToXml() const
{
  std::unique_ptr<rapidxml::xml_document<char>> doc( new rapidxml::xml_document<char>() );
  
  serialize( doc.get() );
  
  return move( doc );
}//std::unique_ptr<rapidxml::xml_document<char>> guiStateToXml() const


void RelActAutoGui::setGuiStateFromXml( const rapidxml::xml_document<char> *doc )
{
  if( !doc )
    throw runtime_error( "RelActAutoGui::setGuiStateFromXml: nullptr passed in." );
  
  const rapidxml::xml_node<char> *base_node = doc->first_node( "RelActCalcAuto" );
  if( !base_node )
    throw runtime_error( "RelActAutoGui::setGuiStateFromXml: couldnt find <RelActCalcAuto> node." );
  
  deSerialize( base_node );
}//void setGuiStateFromXml( const rapidxml::xml_node<char> *node );


void RelActAutoGui::handlePresetChange()
{
  // Right now this function just handles setting GUI to default values, but eventually it will
  //  check if the user is in a modified parameter set, and if so save it to memory, so it can go
  //  back to it later
  
  const int index = m_presets->currentIndex();
  if( index == m_current_preset_index )
    return;
  
  m_render_flags |= RenderActions::UpdateNuclidesPresent;
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  m_render_flags |= RenderActions::ChartToDefaultRange;
  scheduleRender();
  
  if( m_current_preset_index >= static_cast<int>(m_preset_paths.size()) )
    m_previous_presets[m_current_preset_index] = guiStateToXml();
  
  m_current_preset_index = index;
  
  m_nuclides->clear();
  m_energy_ranges->clear();
  
  if( index <= 0 )
  {
    // Clear everything out!
    return;
  }
  
  if( index >= m_preset_paths.size() )
  {
    // TODO: let users download config, or clone them
    
    m_nuclides->clear();
    m_energy_ranges->clear();
    
    const auto iter = m_previous_presets.find(index);
    if( iter == std::end(m_previous_presets) )
    {
      passMessage( "Expected state information for '" + m_presets->currentText().toUTF8()
                   + "' is not available - this is not expected - sorry!", "", WarningWidget::WarningMsgHigh );
      
      return;
    }//if( iter == std::end(m_previous_presets) )
    
    if( !iter->second )
    {
      passMessage( "State information was not previously able to be saved for '"
                  + m_presets->currentText().toUTF8() + "' - this is not expected - sorry!",
                  "", WarningWidget::WarningMsgHigh );
      
      return;
    }//if( !iter->second )
    
    try
    {
      setGuiStateFromXml( iter->second.get() );
    }catch( std::exception &e )
    {
      passMessage( "Error de-serializing tool state: " + string(e.what()),
                  "", WarningWidget::WarningMsgHigh );
    }
    
    return;
  }//if( index >= m_preset_paths.size() )
  
  assert( index < m_preset_paths.size() && (index > 0) );
  if( index >= m_preset_paths.size() || (index <= 0) )
    throw runtime_error( "RelActAutoGui::handlePresetChange: invalid selection index " );

  unique_ptr<rapidxml::file<char>> input_file; // define file out here to keep in scope for catch
  
  try
  {
    const string xml_path = m_preset_paths[index];
    input_file.reset( new rapidxml::file<char>( xml_path.c_str() ) );
    rapidxml::xml_document<char> doc;
    doc.parse<rapidxml::parse_trim_whitespace>( input_file->data() );
   
    setGuiStateFromXml( &doc );
  }catch( rapidxml::parse_error &e )
  {
    string msg = "Error parsing preset XML: " + string(e.what());
    const char * const position = e.where<char>();
    if( position && *position )
    {
      const char *end_pos = position;
      for( size_t i = 0; (*end_pos) && (i < 80); ++i )
        end_pos += 1;
      msg += "<br />&nbsp;&nbsp;At: " + std::string(position, end_pos);
    }//if( position )
    
    passMessage( msg, "", WarningWidget::WarningMsgHigh );
  }catch( std::exception &e )
  {
    passMessage( "Error loading preset: " + string(e.what()), "", WarningWidget::WarningMsgHigh );
  }//try / cat to read the XML
}//void RelActAutoGui::handlePresetChange()


void RelActAutoGui::handleRelEffEqnFormChanged()
{
  // blah blah blah
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleRelEffEqnFormChanged();


void RelActAutoGui::handleRelEffEqnOrderChanged()
{
  // blah blah blah
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleRelEffEqnOrderChanged();


void RelActAutoGui::handleFwhmFormChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleFwhmFormChanged()


void RelActAutoGui::handleFitEnergyCalChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateFitEnergyCal;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleFitEnergyCalChanged();


void RelActAutoGui::handleBackgroundSubtractChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}


void RelActAutoGui::handleSameAgeChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}

void RelActAutoGui::handleUPuByCorrelationChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}


void RelActAutoGui::handleNucDataSrcChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleNucDataSrcChanged()


void RelActAutoGui::handleNuclidesChanged()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateNuclidesPresent;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleNuclidesChanged()


void RelActAutoGui::handleNuclidesInfoEdited()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleNuclidesInfoEdited()


void RelActAutoGui::handleEnergyRangeChange()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleEnergyRangeChange()


void RelActAutoGui::handleFreePeakChange()
{
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateFreePeaks;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleFreePeakChange()


void RelActAutoGui::setOptionsForNoSolution()
{
  makeZeroAmplitudeRoisToChart();
  
  for( WWidget *w : m_energy_ranges->children() )
  {
    RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>( w );
    assert( roi );
    if( roi )
      roi->enableSplitToIndividualRanges( false );
  }//for( WWidget *w : kids )
  
}//void setOptionsForNoSolution()


void RelActAutoGui::setOptionsForValidSolution()
{
  assert( m_solution && (m_solution->m_status == RelActCalcAuto::RelActAutoSolution::Status::Success) );
  if( !m_solution || (m_solution->m_status != RelActCalcAuto::RelActAutoSolution::Status::Success) )
    return;
    
  for( WWidget *w : m_energy_ranges->children() )
  {
    RelActAutoEnergyRange *roi = dynamic_cast<RelActAutoEnergyRange *>( w );
    assert( roi );
    if( !roi )
      continue;
    
    const float lower_energy = roi->lowerEnergy();
    const float upper_energy = roi->upperEnergy();
    
    // Only enable splitting the energy range if it will split into more than one sub-range
    size_t num_sub_ranges = 0;
    for( const RelActCalcAuto::RoiRange &range : m_solution->m_final_roi_ranges )
    {
      const double mid_energy = 0.5*(range.lower_energy + range.upper_energy);
      num_sub_ranges += ((mid_energy > lower_energy) && (mid_energy < upper_energy));
    }
    
    roi->enableSplitToIndividualRanges( (num_sub_ranges > 1) );
  }//for( WWidget *w : kids )
  
}//void setOptionsForValidSolution()


void RelActAutoGui::makeZeroAmplitudeRoisToChart()
{
  m_peak_model->setPeaks( vector<PeakDef>{} );
  const vector<RelActCalcAuto::RoiRange> rois = getRoiRanges();
  
  if( !m_foreground )
    return;
  
  vector<shared_ptr<const PeakDef>> peaks;
  for( const auto &roi : rois )
  {
    try
    {
      const double mean = 0.5*(roi.lower_energy + roi.upper_energy);
      const double sigma = 0.5*fabs( roi.upper_energy - roi.lower_energy );
      const double amplitude = 0.0;
      
      auto peak = make_shared<PeakDef>(mean, sigma, amplitude );
      
      peak->continuum()->setRange( roi.lower_energy, roi.upper_energy );
      peak->continuum()->calc_linear_continuum_eqn( m_foreground, mean, roi.lower_energy, roi.upper_energy, 3, 3 );
      
      peaks.push_back( peak );
    }catch( std::exception &e )
    {
      m_spectrum->updateRoiBeingDragged( {} );
      cerr << "RelActAutoGui::makeZeroAmplitudeRoisToChart caught exception: " << e.what() << endl;
      return;
    }//try / catch
  }//for( const auto &roi : rois )
  
  m_peak_model->setPeaks( peaks );
}//void RelActAutoGui::makeZeroAmplitudeRoisToChart()


void RelActAutoGui::handleAddNuclide()
{
  const vector<WWidget *> prev_kids = m_nuclides->children();
  
  RelActAutoNuclide *nuc_widget = new RelActAutoNuclide( this, m_nuclides );
  nuc_widget->updated().connect( this, &RelActAutoGui::handleNuclidesChanged );
  nuc_widget->remove().connect( boost::bind( &RelActAutoGui::handleRemoveNuclide,
                                              this, static_cast<WWidget *>(nuc_widget) ) );
  nuc_widget->setNuclideEditFocus();
  
  shared_ptr<const ColorTheme> theme = InterSpec::instance()->getColorTheme();
  if( theme )
  {
    vector<WColor> colors = theme->referenceLineColor;
    
    for( WWidget *w : prev_kids )
    {
      RelActAutoNuclide *nuc = dynamic_cast<RelActAutoNuclide *>( w );
      assert( nuc );
      if( !nuc )
        continue;
      
      const WColor color = nuc->color();
      const auto pos = std::find( begin(colors), end(colors), color );
      if( pos != end(colors) )
        colors.erase( pos );
    }
    
    if( colors.size() )
      nuc_widget->setColor( colors.front() );
  }//if( theme )
  
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateNuclidesPresent;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleAddNuclide()


void RelActAutoGui::handleAddEnergy()
{
  const int nprev = m_energy_ranges->count();
  
  RelActAutoEnergyRange *energy_range = new RelActAutoEnergyRange( this, m_energy_ranges );
  
  energy_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
  
  energy_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                      this, static_cast<WWidget *>(energy_range) ) );
  
  if( nprev == 0 )
  {
    const auto cal = m_foreground ? m_foreground->energy_calibration() : nullptr;
    const float upper_energy = (cal && cal->valid()) ? cal->upper_energy() : 3000.0f;
    energy_range->setEnergyRange( 125.0f, upper_energy );
  }else
  {
    energy_range->setForceFullRange( true );
  }//if( this is the first energy range ) / else
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleAddEnergy()


void RelActAutoGui::handleShowFreePeaks()
{
  m_show_free_peak->hide();
  m_free_peaks_container->show();
}//void handleShowFreePeaks()


void RelActAutoGui::handleHideFreePeaks()
{
  const int nfree_peaks = m_free_peaks->count();
  m_free_peaks->clear();
  m_show_free_peak->show();
  m_free_peaks_container->hide();
  
  if( nfree_peaks )
  {
    checkIfInUserConfigOrCreateOne();
    m_render_flags |= RenderActions::UpdateCalculations;
    m_render_flags |= RenderActions::UpdateFreePeaks;
    scheduleRender();
  }
}//void handleHideFreePeaks()


void RelActAutoGui::handleRemoveFreePeak( Wt::WWidget *w )
{
  if( !w )
    return;
  
  const std::vector<WWidget *> &kids = m_free_peaks->children();
  const auto pos = std::find( begin(kids), end(kids), w );
  if( pos == end(kids) )
  {
    cerr << "Failed to find a RelActFreePeak in m_free_peaks!" << endl;
    assert( 0 );
    return;
  }
  
  assert( dynamic_cast<RelActFreePeak *>(w) );
  
  delete w;
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleRemoveFreePeak( Wt::WWidget *w )


void RelActAutoGui::handleRemoveEnergy( WWidget *w )
{
  if( !w )
    return;
  
  const std::vector<WWidget *> &kids = m_energy_ranges->children();
  const auto pos = std::find( begin(kids), end(kids), w );
  if( pos == end(kids) )
  {
    cerr << "Failed to find a RelActAutoEnergyRange in m_energy_ranges!" << endl;
    assert( 0 );
    return;
  }
  
  assert( dynamic_cast<RelActAutoEnergyRange *>(w) );
  
  delete w;
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateEnergyRanges;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleRemoveEnergy( Wt::WContainerWidget *w )


void RelActAutoGui::handleSplitEnergyRange( Wt::WWidget *w, const double energy )
{
  handleRemovePartOfEnergyRange( w, energy, energy );
}


void RelActAutoGui::handleConvertEnergyRangeToIndividuals( Wt::WWidget *w )
{
  RelActAutoEnergyRange *energy_range = dynamic_cast<RelActAutoEnergyRange *>(w);
  assert( energy_range );
  
  const shared_ptr<const RelActCalcAuto::RelActAutoSolution> solution = m_solution;
  if( !solution || (solution->m_status != RelActCalcAuto::RelActAutoSolution::Status::Success) )
  {
    // TODO: just hide/disable the button untill we have a valid solution
    SimpleDialog *dialog = new SimpleDialog( "Can't perform this action.",
                                            "Sorry, a valid solution is needed before an energy range can be split." );
    dialog->addButton( "Continue" );
    
    return;
  }//if( !solution || (solution->m_status != RelActCalcAuto::RelActAutoSolution::Status::Success) )
  
  if( !energy_range || energy_range->isEmpty() )
  {
    SimpleDialog *dialog = new SimpleDialog( "Can't perform this action.",
                                            "Sorry, energy range is currently not valid." );
    dialog->addButton( "Continue" );
    return;
  }
  
  const float lower_energy = energy_range->lowerEnergy();
  const float upper_energy = energy_range->upperEnergy();
  
  vector<RelActCalcAuto::RoiRange> to_ranges;
  for( const RelActCalcAuto::RoiRange &range : solution->m_final_roi_ranges )
  {
    // If the center of `range` falls between `lower_energy` and `upper_energy`, we'll
    //  assume its a match.  This is strictly true, as `range.allow_expand_for_peak_width`
    //  could be true, and/or another ROI can slightly overlap the original one we are
    //  interested in.
    //  TODO: improve the robustness of the matching between the initial ROI, and auto-split ROIs
    
    const double mid_energy = 0.5*(range.lower_energy + range.upper_energy);
    //range.continuum_type = PeakContinuum::OffsetType::;
    //range.force_full_range = false;
    //range.allow_expand_for_peak_width = false;
    
    if( (mid_energy > lower_energy) && (mid_energy < upper_energy) )
      to_ranges.push_back( range );
  }//for( loop over m_final_roi_ranges )
  
  
  // We'll sort the ranges into reverse energy order so when we insert them at a fixed index,
  //  they will be in increasing energy order.
  std::sort( begin(to_ranges), end(to_ranges), []( const RelActCalcAuto::RoiRange &lhs, const RelActCalcAuto::RoiRange &rhs ) -> bool {
    return (lhs.lower_energy + lhs.upper_energy) > (rhs.lower_energy + rhs.upper_energy);
  } );
  
  
  char buffer[512] = { '\0' };
  if( to_ranges.empty() )
  {
    snprintf( buffer, sizeof(buffer),
             "The energy range %.1f keV to %.1f keV did not contain any significant gamma contributions.",
             lower_energy, upper_energy);
    SimpleDialog *dialog = new SimpleDialog( "Can't perform this action.", buffer );
    dialog->addButton( "Continue" );
    return;
  }//if( to_ranges.empty() )
  

  
  snprintf( buffer, sizeof(buffer),
           "This action will divide the energy range from %.1f keV to %.1f keV into %i seperate energy ranges.<br />"
           "<p>Would you like to continue?</p>",
           lower_energy, upper_energy, static_cast<int>(to_ranges.size()) );
  
  SimpleDialog *dialog = new SimpleDialog( "Divide Energy Range Up?", buffer );
  WPushButton *yes_button = dialog->addButton( "Yes" );
  dialog->addButton( "No" );
  
  
  const auto on_yes = [this,w,to_ranges](){
    
    const std::vector<WWidget *> &kids = m_energy_ranges->children();
    const auto pos = std::find( begin(kids), end(kids), w );
    if( pos == end(kids) )
    {
      SimpleDialog *dialog = new SimpleDialog( "Error", "There was an unexpected error finding the original"
                                              " energy range - sorry, cant complete operation." );
      dialog->addButton( "Continue" );
      return;
    }//
    
    const int orig_w_index = pos - begin(kids);
    
    delete w;
    
    for( const RelActCalcAuto::RoiRange &range : to_ranges )
    {
      RelActAutoEnergyRange *roi = new RelActAutoEnergyRange( this );
      roi->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
      roi->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy, this, static_cast<WWidget *>(roi) ) );
      
      roi->setFromRoiRange( range );
      roi->setForceFullRange( true );
      
      m_energy_ranges->insertWidget( orig_w_index, roi );
    }//for( const RelActCalcAuto::RoiRange &range : to_ranges )
    
    checkIfInUserConfigOrCreateOne();
    m_render_flags |= RenderActions::UpdateEnergyRanges;
    m_render_flags |= RenderActions::UpdateCalculations;
    scheduleRender();
  };//on_yes lamda
  
  
  yes_button->clicked().connect( std::bind(on_yes) );
}//void RelActAutoGui::handleConvertEnergyRangeToIndividuals( Wt::WWidget *w )


void RelActAutoGui::handleAddFreePeak( const double energy, const bool constrain_fwhm, const bool apply_cal )
{
  if( m_free_peaks_container->isHidden() )
    handleShowFreePeaks();
  
  RelActFreePeak *peak = new RelActFreePeak( this, m_free_peaks );
  peak->updated().connect( this, &RelActAutoGui::handleFreePeakChange );
  peak->remove().connect( boost::bind( &RelActAutoGui::handleRemoveFreePeak, this, static_cast<WWidget *>(peak) ) );
  peak->setEnergy( static_cast<float>(energy) );
  peak->setFwhmConstrained( constrain_fwhm );
  peak->setApplyEnergyCal( apply_cal );
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= UpdateFreePeaks;
  scheduleRender();
}//void handleAddFreePeak( const double energy, const bool constrain_fwhm )


void RelActAutoGui::handleRemovePartOfEnergyRange( Wt::WWidget *w,
                                                  double lower_energy,
                                                  double upper_energy )
{
  if( !w )
    return;
 
  if( upper_energy < lower_energy )
    std::swap( lower_energy, upper_energy );
  
  const std::vector<WWidget *> &kids = m_energy_ranges->children();
  const auto pos = std::find( begin(kids), end(kids), w );
  if( pos == end(kids) )
  {
    cerr << "Failed to find a RelActAutoEnergyRange in m_energy_ranges (handleRemovePartOfEnergyRange)!" << endl;
    assert( 0 );
    return;
  }
  
  RelActAutoEnergyRange *range = dynamic_cast<RelActAutoEnergyRange *>( w );
  assert( range );
  if( !range )
    return;
  
  RelActCalcAuto::RoiRange roi = range->toRoiRange();
  
  if( (upper_energy < roi.lower_energy) || (lower_energy > roi.upper_energy) )
  {
    assert( 0 );
    return;
  }
  
  delete w;
  
  // Check if we want the whole energy range removed
  if( (lower_energy <= roi.lower_energy) && (upper_energy >= roi.upper_energy) )
  {
    // TODO: remove peaks from ROI from PeakModel
    handleEnergyRangeChange();
    return;
  }
  
  const bool is_in_middle = ((upper_energy < roi.upper_energy) && (lower_energy > roi.lower_energy));
  const bool is_left = ((upper_energy > roi.lower_energy) && (lower_energy <= roi.lower_energy));
  const bool is_right = ((lower_energy > roi.lower_energy) && (upper_energy >= roi.upper_energy));
  
  assert( is_in_middle || is_left || is_right );
  assert( (int(is_in_middle) + int(is_left) + int(is_right)) == 1 );
  
  
  const int orig_w_index = pos - begin(kids);
  
  if( is_in_middle )
  {
    RelActAutoEnergyRange *left_range = new RelActAutoEnergyRange( this );
    left_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    left_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                              this, static_cast<WWidget *>(left_range) ) );
    
    RelActAutoEnergyRange *right_range = new RelActAutoEnergyRange( this );
    right_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    right_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                               this, static_cast<WWidget *>(right_range) ) );
    
    left_range->setFromRoiRange( roi );
    right_range->setFromRoiRange( roi );
    left_range->setEnergyRange( roi.lower_energy, lower_energy );
    right_range->setEnergyRange( upper_energy, roi.upper_energy );
    
    m_energy_ranges->insertWidget( orig_w_index, right_range );
    m_energy_ranges->insertWidget( orig_w_index, left_range );
    
    // TODO: we could split PeakModels ROI peaks here and set them to provide instant feedback during computation
  }else if( is_left )
  {
    RelActAutoEnergyRange *right_range = new RelActAutoEnergyRange( this );
    right_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    right_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                               this, static_cast<WWidget *>(right_range) ) );
    right_range->setEnergyRange( upper_energy, roi.upper_energy );
    right_range->setFromRoiRange( roi );
    m_energy_ranges->insertWidget( orig_w_index, right_range );
    
    // TODO: we could update PeakModels peaks/range here and set them to provide instant feedback during computation
  }else if( is_right )
  {
    RelActAutoEnergyRange *left_range = new RelActAutoEnergyRange( this );
    left_range->updated().connect( this, &RelActAutoGui::handleEnergyRangeChange );
    left_range->remove().connect( boost::bind( &RelActAutoGui::handleRemoveEnergy,
                                              this, static_cast<WWidget *>(left_range) ) );
    left_range->setFromRoiRange( roi );
    left_range->setEnergyRange( roi.lower_energy, lower_energy );
    m_energy_ranges->insertWidget( orig_w_index, left_range );
    
    // TODO: we could update PeakModels peaks/range here and set them to provide instant feedback during computation
  }
  
  handleEnergyRangeChange();
}//void handleSplitEnergyRange( Wt::WWidget *energy_range, const double energy )


void RelActAutoGui::handleRemoveNuclide( Wt::WWidget *w )
{
  if( !w )
    return;
  
  assert( dynamic_cast<RelActAutoNuclide *>(w) );
  
  const std::vector<WWidget *> &kids = m_nuclides->children();
  const auto pos = std::find( begin(kids), end(kids), w );
  if( pos == end(kids) )
  {
    cerr << "Failed to find a RelActAutoNuclide in m_nuclides!" << endl;
    assert( 0 );
    return;
  }
  
  delete w;
  
  checkIfInUserConfigOrCreateOne();
  m_render_flags |= RenderActions::UpdateNuclidesPresent;
  m_render_flags |= RenderActions::UpdateCalculations;
  scheduleRender();
}//void handleRemoveNuclide( Wt::WWidget *w )


void RelActAutoGui::updateDuringRenderForSpectrumChange()
{
  const shared_ptr<const SpecUtils::Measurement> fore
                   = m_interspec->displayedHistogram( SpecUtils::SpectrumType::Foreground );
  const shared_ptr<const SpecUtils::Measurement> back
                   = m_interspec->displayedHistogram( SpecUtils::SpectrumType::Background );
  
  const bool foreground_same = (fore == m_foreground);
  
  m_foreground = fore;
  m_background = back;
  if( m_background )
    m_background_sf = m_interspec->displayScaleFactor( SpecUtils::SpectrumType::Background );
  else
    m_background_sf = 1.0;
  
  m_spectrum->setData( m_foreground, foreground_same );
  m_spectrum->setBackground( m_background );
  m_spectrum->setDisplayScaleFactor( m_background_sf, SpecUtils::SpectrumType::Background );
  
  m_spectrum->removeAllDecorativeHighlightRegions();
  
  m_background_subtract->setHidden( !m_background );
}//void updateDuringRenderForSpectrumChange()


void RelActAutoGui::updateSpectrumToDefaultEnergyRange()
{
  if( !m_foreground || (m_foreground->gamma_energy_max() < 1.0f) )
  {
    m_spectrum->setXAxisRange( 0, 3000 );
    return;
  }
  
  const double spec_min = m_foreground->gamma_energy_min();
  const double spec_max = m_foreground->gamma_energy_max();
  
  const vector<RelActCalcAuto::RoiRange> rois = getRoiRanges();
  if( rois.empty() )
  {
    m_spectrum->setXAxisRange( spec_min, spec_max );
    return;
  }
  
  double min_energy = 3000, max_energy = 0;
  for( const RelActCalcAuto::RoiRange &roi : rois )
  {
    min_energy = std::min( min_energy, roi.lower_energy );
    max_energy = std::max( max_energy, roi.upper_energy );
  }
  
  //Make sure max energy is greater than min energy by at least ~10 keV; this is arbitrary, but we
  //  dont ant the spectrum hyper-zoomed-in to like a single channel
  if( (max_energy > min_energy) && ((max_energy - min_energy) > 10.0) )
  {
    const double range = max_energy - min_energy;
    min_energy -= 0.1*range;
    max_energy += 0.1*range;
    
    min_energy = std::max( min_energy, spec_min );
    max_energy = std::min( max_energy, spec_max );
    
    m_spectrum->setXAxisRange( min_energy, max_energy );
  }else
  {
    m_spectrum->setXAxisRange( spec_min, spec_max );
  }
}//void updateSpectrumToDefaultEnergyRange()


void RelActAutoGui::updateDuringRenderForNuclideChange()
{
  // Check if we need to show/hide
  //  - m_same_z_age
  //  - m_u_pu_by_correlation - and also update its text
  //  - m_u_pu_data_source
  //
  // Need to check nuclides arent duplicated
  // Make sure "Fit Age" checkbox is in a correct state.  I.e.
  //  - make sure if each nuclide *could* have ages fit, that there is peaks from at least two progeny in the used energy ranges
  //  - make sure "Same El Same Age" is checked, then all nuclide ages for an element have same age and same m_fit_age value
  
  
  // For a first go, we'll show #m_same_z_age if we hadve more than one nuclide for a given Z.
  //   However, we really need to be more complex about this, and also hide "Fit Age" and disable age input if this option is selected, on all but the first entry of this Z
  bool has_multiple_nucs_of_z = false;
  map<short,int> z_to_num_isotopes;
  const vector<RelActCalcAuto::NucInputInfo> nuclides = getNucInputInfo();
  for( const RelActCalcAuto::NucInputInfo &nuc : nuclides )
  {
    if( !nuc.nuclide )
      continue;
    
    const short z = nuc.nuclide->atomicNumber;
    if( !z_to_num_isotopes.count(z) )
      z_to_num_isotopes[z] = 0;
    
    int &num_this_z = z_to_num_isotopes[z];
    num_this_z += 1;
    
    has_multiple_nucs_of_z = has_multiple_nucs_of_z || (num_this_z > 1);
  }//for( const RelActCalcAuto::NucInputInfo &nuc : nuclides )
  
  if( m_same_z_age->isVisible() != has_multiple_nucs_of_z )
    m_same_z_age->setHidden( !has_multiple_nucs_of_z );
}//void updateDuringRenderForNuclideChange()


void RelActAutoGui::updateDuringRenderForFreePeakChange()
{
  // Check that free peaks are within ROIs, and if not, mark them as such

  if( m_free_peaks_container->isHidden() )
    return;
  
  vector<pair<float,float>> rois;
  const vector<WWidget *> &roi_widgets = m_energy_ranges->children();
  for( WWidget *w : roi_widgets )
  {
    const RelActAutoEnergyRange *roi = dynamic_cast<const RelActAutoEnergyRange *>( w );
    assert( roi );
    if( roi && !roi->isEmpty() )
      rois.emplace_back( roi->lowerEnergy(), roi->upperEnergy() );
  }//for( WWidget *w : kids )
  
  
  const bool fit_energy_cal = m_fit_energy_cal->isChecked();
  
  const std::vector<WWidget *> &kids = m_free_peaks->children();
  for( WWidget *w : kids )
  {
    RelActFreePeak *free_peak = dynamic_cast<RelActFreePeak *>(w);
    assert( free_peak );
    if( !free_peak )
      continue;
    
    const float energy = free_peak->energy();
    bool in_roi = false;
    for( const auto &roi : rois )
      in_roi = (in_roi || ((energy >= roi.first) && (energy <= roi.second)));
    
    free_peak->setInvalidEnergy( !in_roi );
    free_peak->setApplyEnergyCalVisible( fit_energy_cal );
  }//for( loop over RelActFreePeak widgets )
}//void updateDuringRenderForFreePeakChange()


void RelActAutoGui::updateDuringRenderForEnergyRangeChange()
{
  // Check if we need to show/hide/edit:
  // - m_presets
  // - m_rel_eff_eqn_order
  // - m_fwhm_eqn_form
  //
  // Need to make sure energy ranges arent overlapping tooo much
  
}//void updateDuringRenderForEnergyRangeChange()


void RelActAutoGui::startUpdatingCalculation()
{
  m_error_msg->setText("");
  m_error_msg->hide();
  m_status_indicator->hide();
  
  shared_ptr<const SpecUtils::Measurement> foreground = m_foreground;
  shared_ptr<const SpecUtils::Measurement> background = m_background;
  RelActCalcAuto::Options options;
  vector<RelActCalcAuto::RoiRange> rois;
  vector<RelActCalcAuto::NucInputInfo> nuclides;
  vector<RelActCalcAuto::FloatingPeak> floating_peaks;
  
  try
  {
    if( !foreground )
      throw runtime_error( "No foreground spectrum is displayed." );
    
    if( !m_solution || !m_peak_model->rowCount() )
      makeZeroAmplitudeRoisToChart();
    
    if( m_background_subtract->isChecked() )
      background = m_interspec->displayedHistogram( SpecUtils::SpectrumType::Background );
    
    options = getCalcOptions();
    rois = getRoiRanges();
    nuclides = getNucInputInfo();
    floating_peaks = getFloatingPeaks();
    
    if( nuclides.empty() && rois.empty() )
      throw runtime_error( "No energy ranges or nuclides defined." );
    else if( nuclides.empty() )
      throw runtime_error( "No nuclides defined." );
    else if( rois.empty() )
      throw runtime_error( "No energy ranges defined." );
    
  }catch( std::exception &e )
  {
    m_is_calculating = false;
    m_error_msg->setText( e.what() );
    m_error_msg->show();
    
    setOptionsForNoSolution();
    
    return;
  }//try / catch
  
  m_status_indicator->setText( "Calculating..." );
  m_status_indicator->show();
  
  if( m_cancel_calc )
    m_cancel_calc->store( true );
  
  const string sessionid = wApp->sessionId();
  m_is_calculating = true;
  m_cancel_calc = make_shared<std::atomic_bool>();
  m_cancel_calc->store( false );
  shared_ptr<atomic_bool> cancel_calc = m_cancel_calc;
  
  shared_ptr<const DetectorPeakResponse> cached_drf = m_cached_drf;
  if( !cached_drf )
  {
    auto m = m_interspec->measurment(SpecUtils::SpectrumType::Foreground);
    if( m && m->detector() && m->detector()->isValid() && m->detector()->hasResolutionInfo() )
      cached_drf = m->detector();
  }
  
  WApplication *app = WApplication::instance();
  
  vector<shared_ptr<const PeakDef>> cached_all_peaks = m_cached_all_peaks;
  
  auto solution = make_shared<RelActCalcAuto::RelActAutoSolution>();
  auto error_msg = make_shared<string>();
  
  auto gui_update_callback = wApp->bind( boost::bind( &RelActAutoGui::updateFromCalc, this, solution, cancel_calc) );
  auto error_callback = wApp->bind( boost::bind( &RelActAutoGui::handleCalcException, this, error_msg, cancel_calc) );
  
  
  auto worker = [=](){
    try
    {
      RelActCalcAuto::RelActAutoSolution answer
        = RelActCalcAuto::solve( options, rois, nuclides, floating_peaks,
                                foreground, background, cached_drf,
                                cached_all_peaks, cancel_calc );
      
      WApplication::UpdateLock lock( app );
      if( lock )
      {
        *solution = answer;
        gui_update_callback();
        app->triggerUpdate();
      }else
      {
        cerr << "Failed to get WApplication::UpdateLock for worker in RelActAutoGui::startUpdatingCalculation" << endl;
        assert( 0 );
      }//if( lock ) / else
    }catch( std::exception &e )
    {
      WApplication::UpdateLock lock( app );
      if( lock )
      {
        *error_msg = e.what();
        error_callback();
        app->triggerUpdate();
      }else
      {
        cerr << "Failed to get WApplication::UpdateLock for worker in RelActAutoGui::startUpdatingCalculation" << endl;
        assert( 0 );
      }//if( lock ) / else
    }//try / catch
  };//auto worker
  
  WServer::instance()->ioService().boost::asio::io_service::post( worker );
}//void startUpdatingCalculation()


void RelActAutoGui::updateFromCalc( std::shared_ptr<RelActCalcAuto::RelActAutoSolution> answer,
                                    std::shared_ptr<std::atomic_bool> cancel_flag )
{
  assert( answer );
  if( !answer )
    return;
  
  // If we started a new calculation between when this one was started, and right now, dont
  //  do anything to the GUI state.
  if( cancel_flag != m_cancel_calc )
    return;
  
  m_is_calculating = false;
  m_status_indicator->hide();
  
  switch( answer->m_status )
  {
    case RelActCalcAuto::RelActAutoSolution::Status::Success:
      break;
      
    case RelActCalcAuto::RelActAutoSolution::Status::NotInitiated:
    case RelActCalcAuto::RelActAutoSolution::Status::FailedToSetupProblem:
    case RelActCalcAuto::RelActAutoSolution::Status::FailToSolveProblem:
    case RelActCalcAuto::RelActAutoSolution::Status::UserCanceled:
    {
      string msg = "Calculation didn't complete.";
      if( !answer->m_error_message.empty() )
        msg += ": " + answer->m_error_message;
      
      m_error_msg->setText( msg );
      m_error_msg->show();
      m_txt_results->setNoResults();
      
      setOptionsForNoSolution();
      
      return;
    }//if( calculation wasnt successful )
  }//switch( answer->m_status )
  
  if( answer->m_drf )
    m_cached_drf = answer->m_drf;
  
  if( !answer->m_spectrum_peaks.empty() )
    m_cached_all_peaks = answer->m_spectrum_peaks;
  
  m_solution = answer;
  
  m_txt_results->updateResults( *answer );
  
  m_peak_model->setPeaks( answer->m_fit_peaks );
  
  cout << "\n\n\nCalc finished: \n";
  answer->print_summary( std::cout );
  cout << "\n\n\n";
  
  const string rel_eff_eqn_js = RelActCalc::rel_eff_eqn_js_function( answer->m_rel_eff_form,
                                                                  answer->m_rel_eff_coefficients );
  
  const double live_time = answer->m_foreground ? answer->m_foreground->live_time() : 1.0f;
  
  m_rel_eff_chart->setData( live_time, answer->m_fit_peaks, answer->m_rel_activities, rel_eff_eqn_js );
  
  setOptionsForValidSolution();
}//void updateFromCalc( std::shared_ptr<RelActCalcAuto::RelActAutoSolution> answer )


void RelActAutoGui::handleCalcException( std::shared_ptr<std::string> message,
                                        std::shared_ptr<std::atomic_bool> cancel_flag )
{
  assert( message );
  if( !message )
    return;
  
  // If we started a new calculation between when this one was started, and right now, dont
  //  do anything to the GUI state.
  if( cancel_flag != m_cancel_calc )
    return;
  
  m_is_calculating = false;
  m_status_indicator->hide();
  
  string msg = "Calculation error: ";
  msg += *message;
  
  m_error_msg->setText( msg );
  m_error_msg->show();
  
  setOptionsForNoSolution();
}//void handleCalcException( std::shared_ptr<std::string> message )
