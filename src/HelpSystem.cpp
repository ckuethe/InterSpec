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
#include <fstream>

#include <Wt/WText>
#include <Wt/WTree>
#include <Wt/WTimer>
#include <Wt/WString>
#include <Wt/WTemplate>
#include <Wt/WTreeNode>
#include <Wt/WLineEdit>
#include <Wt/Json/Value>
#include <Wt/Json/Array>
#include <Wt/WPushButton>
#include <Wt/WGridLayout>
#include <Wt/Json/Object>
#include <Wt/Json/Parser>
#include <Wt/WImage>
#include <Wt/WContainerWidget>
#include <Wt/WMessageResourceBundle>

#include "InterSpec/HelpSystem.h"
#include "InterSpec/InterSpec.h"
#include "InterSpec/InterSpecApp.h"
#include "SpecUtils/UtilityFunctions.h"

#if( USE_OSX_NATIVE_MENU )
#include "InterSpec/PopupDiv.h"
#include "target/osx/NativeMenu.h"
#endif


using namespace std;
using namespace Wt;

//TODO: Remove pNotify/javascript usage below if we are to move fully to new qTip2 method

//HelpPopupOnMouseOver, HelpPopupMouseOutOfPopup, and HelpPopupMouseOverPopup
//  all share most of their code and could be refactored a bit.
//
//Note, could replace instances of p.data('focus') with somethign like:
//  "Wt.WT.hasFocus(" + widget->jsRef() + ")"
//
//Note: Line to hide all other popups could be replaced with:
//      "$(" + wApp->domRoot()->jsRef() + ").children('.helpPopupMainContainer').each(...)"
//      or "$('.helpPopupMainContainer').each(function(i,el){jQuery.data(el,'popup').hide();});"
WT_DECLARE_WT_MEMBER
(HelpPopupOnFocus, Wt::JavaScriptFunction, "HelpPopupOnFocus",
 function(a,s)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('focus',true);
  if(pp&&!p.data('StayHidden'))
  {
    p.parent().children('.helpPopupMainContainer').each(function(i,el){if(a.id!=el.id)jQuery.data(el,'popup').hide();});
    pp.show(s,Wt.WT.Horizontal);
  }
}
 );


WT_DECLARE_WT_MEMBER
(HelpPopupOnBlurr, Wt::JavaScriptFunction, "HelpPopupOnBlurr",
 function(a)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('focus',false);
  p.data('StayHidden',false);
  var fcn=function(){if(!p.data('override')&&!p.data('focus')&&!p.data('mouse'))pp.hide();};
  setTimeout(fcn,500);
}
 );


WT_DECLARE_WT_MEMBER
(HelpPopupOnMouseOver, Wt::JavaScriptFunction, "HelpPopupOnMouseOver",
 function(a,s)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('mouse',true);
  if(pp&&!p.data('StayHidden'))
  {
    p.parent().children('.helpPopupMainContainer').each(function(i,el){if(a.id!=el.id)jQuery.data(el,'popup').hide();});
    pp.show(s,Wt.WT.Horizontal);
  }
}
 );


WT_DECLARE_WT_MEMBER
(HelpPopupOnMouseOut, Wt::JavaScriptFunction, "HelpPopupOnMouseOut",
 function(a)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('mouse',false);
  var fcn=function(){if(!p.data('override')&&!p.data('focus')&&!p.data('mouse'))pp.hide();};
  setTimeout(fcn,500);
}
 );

WT_DECLARE_WT_MEMBER
(HelpPopupOnKeyPress, Wt::JavaScriptFunction, "HelpPopupOnKeyPress",
 function(a)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('StayHidden',true);
  pp.hide();
}
 );


WT_DECLARE_WT_MEMBER
(HelpPopupMouseOverPopup, Wt::JavaScriptFunction, "HelpPopupMouseOverPopup",
 function(a)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('override',true);
  var fcn=function(){if(!p.data('override')&&!p.data('focus')&&!p.data('mouse'))pp.hide();};
  setTimeout(fcn,500);
}
 );

WT_DECLARE_WT_MEMBER
(HelpPopupMouseOutOfPopup, Wt::JavaScriptFunction, "HelpPopupMouseOutOfPopup",
 function(a)
{
  var p=$(a);
  var pp=jQuery.data(a,'popup');
  p.data('override',false);
  var fcn=function(){if(!p.data('override')&&!p.data('focus')&&!p.data('mouse'))pp.hide();};
  setTimeout(fcn,500);
}
 );


namespace HelpSystem
{
  void createHelpWindow( const std::string &preselect )
  {
    new HelpWindow( preselect );
  }
  
  HelpWindow::HelpWindow(std::string preselect)
   : AuxWindow( "InterSpec Help",
               (Wt::WFlags<AuxWindowProperties>(AuxWindowProperties::IsAlwaysModal) | AuxWindowProperties::IsHelpWIndow) ),
     m_tree ( new Wt::WTree() ),
     m_searchText( new Wt::WLineEdit() )
  {
    wApp->useStyleSheet( "InterSpec_resources/HelpWindow.css" );
    
    m_tree->setSelectionMode(Wt::SingleSelection);
    
    //To Do add in special
    //"help-xml"
    //"help-xml-desktop"
    //"help-xml-mobile"
    //"help-xml-phone"
    //"help-xml-tablet"
    
    const char *help_json = "InterSpec_resources/static_text/help.json";
    ifstream helpinfo( help_json );
    if( helpinfo.is_open() )
    {
      m_helpLookupTable = string( (std::istreambuf_iterator<char>(helpinfo)),
                                 std::istreambuf_iterator<char>());
    }else
    {
      passMessage( "Could not open help JSON file at '" + string(help_json) + "'", "", 2 );
    }
    
    initialize();
    
    m_helpWindowContent = new WContainerWidget();
    m_helpWindowContent->setPadding(WLength(10,WLength::Pixel));
    
    m_tree->setMargin(WLength(0,WLength::Pixel));
    m_tree->addStyleClass("helpTree"); //both need to scroll
    m_helpWindowContent->addStyleClass("helpTree"); //both need to scroll
    m_tree->itemSelectionChanged().connect(boost::bind( &HelpWindow::selectHelpToShow, this ) );
    
    const bool hasinfo = !preselect.empty() && (m_treeLookup.find(preselect)!=m_treeLookup.end());
    
    if( hasinfo )
    {
      m_tree->select( m_treeLookup[preselect] );
    }else if( !preselect.empty() )
    {
      passMessage( "The help instructions appears to not have an entry for "
                  + preselect, "", 2 );
    }//!hasinfo
    
    contents()->setMargin(0);
    contents()->setPadding(0);
    contents()->keyPressed().connect( this, &HelpWindow::handleArrowPress );
    WGridLayout *layout = stretcher();
    m_searchText->setStyleClass("searchBox");
    m_searchText->setPlaceholderText("Search");
    m_searchText->setMinimumSize(Wt::WLength::Auto, WLength(1.5,Wt::WLength::FontEm));
    m_searchText->setMargin(WLength(5,WLength::Pixel));
//    m_searchText->keyPressed().connect( this, &HelpWindow::initialize );
    m_searchText->keyWentDown().connect( this, &HelpWindow::handleKeyPressInSearch );
//    m_searchText->keyWentUp().connect( this, &HelpWindow::initialize );
    
//    m_searchText->keyWentUp	().connect(std::bind([=] (const Wt::WKeyEvent& e) {
// //      if (e.key()==Wt::Key_Space || e.key()==Wt::Key_Enter || e.key()==Wt::Key_Delete|| e.key()==Wt::Key_Backspace)
//        initialize();
//    }, std::placeholders::_1));

//    m_searchText->keyWentUp().connect(std::bind([=]() {
//      if (m_searchText->text().empty())
//        initialize();
//    }));
    layout->addWidget( m_searchText, 0, 0 );
    layout->addWidget( m_tree, 1, 0 );
    layout->addWidget( m_helpWindowContent, 0, 1, 2, 1 );
    layout->setContentsMargins(0,0,0,0);
    layout->setRowStretch( 1, 1 );
    layout->setColumnResizable( 0, true, WLength(25,WLength::FontEx) );
    
    WContainerWidget *bottom = footer();
  //  bottom->setStyleClass("modal-footer");
    //bottom->resize( WLength::Auto, 50 );
    
    InterSpecApp *app = dynamic_cast<InterSpecApp *>(wApp);
    if( app && app->viewer() )
    {
      if( app->viewer()->isMobile() )
      {
        WPushButton *ancor = new WPushButton( bottom );
        ancor->setText( "Welcome..." );
        ancor->setStyleClass( "CsvLinkBtn" );
        ancor->setFloatSide( Wt::Right );
        ancor->clicked().connect( boost::bind( &InterSpec::showWelcomeDialog, app->viewer(), true ) );
      }
      else
      {
          WAnchor *anchor = new WAnchor( WLink(), "Tutorials and usage hints", bottom );
          anchor->setFloatSide( Wt::Left );
          anchor->addStyleClass( "InfoLink" );
          anchor->clicked().connect( boost::bind( &AuxWindow::hide, this ) );
          anchor->clicked().connect( boost::bind( &InterSpec::showWelcomeDialog, app->viewer(), true ) );
      }
    }//if( app && app->viewer() )
    
    
    WPushButton *ok = addCloseButtonToFooter();
    ok->clicked().connect( boost::bind( &AuxWindow::hide, this ) );
    
    if( !preselect.empty() || (app && app->isMobile()) )  //Keep keyboard from popping up
      ok->setFocus();
    
    finished().connect( boost::bind( &AuxWindow::deleteAuxWindow, this ) );
    rejectWhenEscapePressed();
    setMinimumSize(500,400);
    resizeScaledWindow(0.85, 0.85);
    centerWindow();
    //centerWindowHeavyHanded();  //why not?
    setResizable( true );
    show();
    resizeToFitOnScreen();
  } //void HelpSystem::showHelpDialog()
  
  
  HelpWindow::~HelpWindow()
  {
  }
  
  WTreeNode *first_child( WTreeNode *node )
  {
    const vector<WTreeNode *> &kids = node->childNodes();
    
    for( size_t i = 0; i < kids.size(); ++i )
      if( kids[i]->isVisible() )
        return first_child( kids[i] );
    return node;
  }
  
  void HelpWindow::handleArrowPress( const Wt::WKeyEvent e )
  {
    //This function not yet working!
    const Wt::Key key = e.key();
    
    const set<WTreeNode *> &selected = m_tree->selectedNodes();
    if( selected.empty() )
    {
      m_tree->select( first_child(m_tree->treeRoot()) );
      return;
    }
    
    WTreeNode *current = *selected.begin();
    WTreeNode *parent = current->parentNode();
    
    if( !parent || (parent==m_tree->treeRoot()) )
      return;
    
    const vector<WTreeNode *> &kids = current->childNodes();
    const vector<WTreeNode *> &siblings = parent->childNodes();
    const size_t pos = std::find(siblings.begin(), siblings.end(), current)
                       - siblings.begin();
    
    switch( key )
    {
      case Wt::Key_Left:
      {
        m_tree->select( parent );
        break;
      }//case Wt::Key_Left:
        
      case Wt::Key_Up:
      {
        WTreeNode *toselect = parent;
        
        for( int index = static_cast<int>(pos) - 1; !toselect && index >= 0; --index )
        {
          if( siblings[index]->isVisible() )
            toselect = siblings[index];
        }
        
        m_tree->select( toselect );
      }//case Wt::Key_Up:
        
      case Wt::Key_Right:
      {
        for( size_t i = 0; i < kids.size(); ++i )
        {
          if( kids[i]->isVisible() )
          {
            m_tree->select( kids[i] );
            break;
          }
        }
      }//case Wt::Key_Right:
        
      case Wt::Key_Down:
      {
        WTreeNode *firstkid = first_child( current );
        
        WTreeNode *toselect = (firstkid==current ? (WTreeNode *)0 : firstkid);
        
        for( size_t index = pos+1; !toselect && index < siblings.size(); ++index )
          if( siblings[index]->isVisible() )
            toselect = siblings[index];
      
        if( !toselect && parent->parentNode() )
        {
          const vector<WTreeNode *> &uncles = parent->parentNode()->childNodes();
          const size_t pos = std::find(uncles.begin(), uncles.end(), parent)
                             - uncles.begin();
          for( size_t i = pos; !toselect && i < uncles.size(); ++i )
          {
            if( uncles[i]->isVisible() )
              toselect = uncles[i];
          }
        }
        
        if( toselect )
          m_tree->select( toselect );
        
        break;
      }//case Wt::Key_Down:
        
      default:
        break;
    }//switch( key )
  }//void HelpWindow::handleArrowPress( const Wt::Key key )
  
  void HelpWindow::handleKeyPressInSearch( const Wt::WKeyEvent e )
  {
    switch( e.key() )
    {
      case Wt::Key_Left: case Wt::Key_Up: case Wt::Key_Right: case Wt::Key_Down:
        handleArrowPress( e );
        break;
      
      case Wt::Key_Home: case Wt::Key_Alt: case Wt::Key_Control:
      case Wt::Key_Shift: case Wt::Key_Tab: case Wt::Key_unknown:
      case Wt::Key_PageUp: case Wt::Key_PageDown:
        return;
        
      default:
        initialize();
        break;
    }//switch( e.key() )
  }//void handleKeyPressInSearch( const Wt::WKeyEvent e )
  
  void HelpWindow::initialize()
  {
    Wt::WTreeNode *node = new Wt::WTreeNode("Help");
    m_tree->setTreeRoot( node );
    node->label()->setTextFormat( Wt::PlainText );
    node->setLoadPolicy( Wt::WTreeNode::NextLevelLoading );
    m_tree->select( node ); //select first node
    
    try
    {
      Json::Value jsonResult;
      Json::parse( m_helpLookupTable, jsonResult );
      Json::Array res = jsonResult;
      populateTree( jsonResult, node );
      
      //Lets show the first result
      const vector<WTreeNode *> &kids = node->childNodes();
      if( !m_searchText->text().empty() && !kids.empty() )
        m_tree->select( first_child( kids[0] ) );
    }catch( std::exception &e )
    {
      cerr << "showHelpDialog() caught: " << e.what() << endl
      << "you may want to check help.json" << endl;
    }//try / catch
    
    node->setNodeVisible( false ); //hide the root note, not necessary
    
    m_tree->refresh();
  }//void initialize()
  
  
  
  void HelpWindow::selectHelpToShow()
  {
    m_tree->treeRoot()->setNodeVisible(false); //this is needed to keep the root hidden, otherwise it shows on click
    
    if (m_tree->selectedNodes().size()==0)
      return;
    
    WTreeNode * node = *(m_tree->selectedNodes().begin());
    
    std::map <string, Wt::WTreeNode*>::iterator iter;
    std::string strToReturn;
    
    m_helpWindowContent->clear();
    
    bool found=false;
    for (iter = m_treeLookup.begin(); iter != m_treeLookup.end(); ++iter)
    {
      if (((iter->second))==(node))
      {
        std::string helpStr, filepath;
        auto pos = m_contentLookup.find( iter->first );
        if( pos != end(m_contentLookup) )
          filepath = pos->second;
        UtilityFunctions::trim( filepath );
        
        if( filepath.empty() )
        {
          helpStr = "No help item associated with key '" + iter->first + "'";
        }else
        {
          //We have to grab the contents of this file, and place in helpStr...
          //All paths relative to the curent help.json file, which is in
          // "InterSpec_resources/static_text"
          
          //ToDoL: should remove leading and trailing quotes I guess...
          
          filepath = UtilityFunctions::append_path( "InterSpec_resources/static_text", filepath );
          ifstream infile( filepath.c_str() );
          if( !infile.is_open() )
          {
            helpStr = "Could not open expected file: " + filepath + "";
          }else
          {
            infile.seekg(0, std::ios::end);
            const size_t size = infile.tellg();
            infile.seekg(0);
            
            if( size < 128*1024 )
            {
              helpStr = std::string( size, ' ' );
              infile.read( &(helpStr[0]), size );
            }else
            {
              helpStr = filepath + " is too large a file - not loading contents.";
            }
          }//if( !infile.is_open() ) / else.
        }//if( UtilityFunctions::istarts_with( helpStr, "contents_in_file:" ) )
        
        
        WTemplate *t = new WTemplate( m_helpWindowContent );
        t->setTemplateText( WString(helpStr, UTF8), XHTMLUnsafeText );
        t->setInternalPathEncoding( true );


//        WAnchor *x =new WAnchor("","Jump to bottom",m_helpWindowContent);
//        x->setRefInternalPath("bottom");
//        x->setId("top");
//        t->bindWidget( "top",x  );
//        x =new WAnchor("","Jump to top",m_helpWindowContent);
//        x->setRefInternalPath("top");
//        x->setId("bottom");
//        t->bindWidget( "bottom",x  );
//        
        
        found = true;
        break;
      } //((iter->second))==(node)
    } //for (iter = treeLookup.begin(); iter != treeLookup.end(); ++iter)
    
    if (!found)
    { //return a page not found!
      std::string helpStr = "Help Page Not Found";
      new WTemplate( WString(helpStr), m_helpWindowContent );
    }//if (!found)
    
    //Note: this keeps the new pages scrolled to the top.
    doJavaScript("document.body.scrollTop = document.documentElement.scrollTop = 0;");
  } // void HelpSystem::selectHelpToShow(WTree* tree, WContainerWidget* m_helpWindowContent)
  
  
  void HelpWindow::populateTree(Json::Array &res, WTreeNode* parent)
  {
    InterSpecApp *app = dynamic_cast<InterSpecApp *>(wApp);
    
    const string searchTxt = m_searchText->text().toUTF8();
    const bool searching = (searchTxt.length() > 0);
    
    //go through each children element
    for( size_t i = 0; i < res.size(); ++i )
    { //res[i] is std::vector<Json::Value>
      const Json::Object &info = res[i];
      
      const Json::Value &ident = info.get("id");
      const Json::Value &title = info.get("title");
      const Json::Value &keywords = info.get("keywords");
      const Json::Value &children = info.get("children");
      
      if( ident.isNull() || title.isNull())
      {
        cerr << "found a null key or file string, "
        "you may want to check help.json" << endl;
        continue;
      }//if( id.isNull() || title.isNull())
      
      const WString identifier = ident;
      const WString titlestr = title;
    
      WTreeNode *insertNode = new Wt::WTreeNode( titlestr.toUTF8() );
      
      if( searching )
        insertNode->setHidden(true); //by default hidden, and just show nodes that need to be visible
      
      parent->addChildNode(insertNode);
      parent->expand();
      
      if (!keywords.isNull() && searching)
      {
        const WString keywordstr = keywords;
        
        typedef const boost::iterator_range<std::string::const_iterator> StringRange;
        std::string str1(keywordstr.toUTF8());
        std::string str2(searchTxt);
        UtilityFunctions::trim(str2);
        
        vector< std::string > searchWords;
        UtilityFunctions::split( searchWords, str2, " ," ); //split search terms
        
        bool childHide=false;
        for( size_t i = 0; i < searchWords.size(); ++i )
        {
          const std::string &keyword = searchWords[i];
          if (str1.length()==0 || (str1.length()>0 && !boost::ifind_first(StringRange(str1.begin(), str1.end()), StringRange(keyword.begin(), keyword.end()) )))
          {
            childHide=true;
            break;
          } //if found keyword in keywords
        } //for( std::string keyword : searchWords )
        
        if (!childHide)
          setPathVisible(insertNode);
      }//if (!keywords.isNull() && searching)

      const string identstr = identifier.toUTF8();
      m_treeLookup[identstr] = insertNode; //for future matching
      
      if( app )
      {
        WString xml_file;
        const Json::Value &xml = info.get("help-xml");
        const Json::Value &xml_desktop = info.get("help-xml-desktop");
        const Json::Value &xml_mobile = info.get("help-xml-mobile");
        const Json::Value &xml_phone = info.get("help-xml-phone");
        const Json::Value &xml_tablet = info.get("help-xml-tablet");
        
        if( app->isPhone() )
        {
          if( !xml_phone.isNull() )
            xml_file = xml_phone;
          else if( !xml_mobile.isNull() )
            xml_file = xml_mobile;
          else if( !xml.isNull() )
            xml_file = xml;
        }else if( app->isTablet() || app->isMobile() )
        {
          if( !xml_tablet.isNull() )
            xml_file = xml_tablet;
          else if( !xml_mobile.isNull() )
            xml_file = xml_mobile;
          else if( !xml.isNull() )
            xml_file = xml;
        }else  //desktop
        {
          if( !xml_desktop.isNull() )
            xml_file = xml_desktop;
          else if( !xml.isNull() )
            xml_file = xml;
        }
        
        m_contentLookup[identstr] = xml_file.toUTF8();
      }//if( app )
      
      
      if( !children.isNull() )
      {
        //has children
        Json::Array addNodes = children;
        populateTree(addNodes,insertNode);
      }//if (!children.isNull())
    }//for( size_t i = 0; i < res.size(); ++i )
  }//void populateTree(Json::Array &res, WTreeNode* parent, std::map <string, Wt::WTreeNode*> &treeLookup)
  
  /**
   Recursively set entire path from root to this node to be visible
   **/
  void HelpWindow::setPathVisible(Wt::WTreeNode* parent)
  {
    parent->setHidden(false);
    if (parent->parentNode()!=NULL)
      setPathVisible(parent->parentNode());
  }//  void HelpWindow::setPathVisible(WTreeNode* parent)
  
  //@Deprecated -- no longer using this if we move to qTip2
  void addToolTipToWidget( Wt::WPopupWidget *popup,
                                      Wt::WFormWidget *widget,
                                      const bool closeOnType,
                                      const bool openOnMouseOver )
  {
    WApplication *app = WApplication::instance();
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupOnFocus);
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupOnBlurr);
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupOnMouseOver);
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupOnMouseOut);
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupOnKeyPress);
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupMouseOverPopup);
    LOAD_JAVASCRIPT(app, "src/HelpSystem.cpp", "HelpSystem", wtjsHelpPopupMouseOutOfPopup);
    
    WWidget *w = popup->find( "ToolTip" );
    WContainerWidget *helpPopup = dynamic_cast<WContainerWidget *>( w );
    if( !helpPopup )
      throw std::logic_error( "HelpSystem::addToolTipToWidget called with invalid widget" );
    
    string js = "function(s){Wt.WT.HelpPopupOnFocus("+popup->jsRef()+",s);}";
    widget->focussed().connect( js );
    
    if( openOnMouseOver )
    {
      js = "function(s){Wt.WT.HelpPopupOnMouseOver("+popup->jsRef()+",s);}";
      widget->mouseWentOver().connect( js );
    }
    
    js = "function(){Wt.WT.HelpPopupOnBlurr("+popup->jsRef()+");}";
    widget->blurred().connect( js );
    
    
    if( closeOnType )
    {
      js = "function(){Wt.WT.HelpPopupOnKeyPress("+popup->jsRef()+");}";
      widget->keyPressed().connect( js );
    }//if( closeOnType )
    
    js = "function(){Wt.WT.HelpPopupOnMouseOut("+popup->jsRef()+");}";
    widget->mouseWentOut().connect( js );
    
    js = "function(){Wt.WT.HelpPopupMouseOverPopup("+popup->jsRef()+");}";
    helpPopup->mouseWentOver().connect( js );
    
    js = "function(){Wt.WT.HelpPopupMouseOutOfPopup("+popup->jsRef()+");}";
    helpPopup->mouseWentOut().connect( js );
  }//void addToolTipToWidget...)
  
  
  /*
   Attach a tooltip onto a widget.  Depending on preference, it might show immediately.
   
   */
  void attachToolTipOn( Wt::WWebWidget* widget, const std::string &text, const bool showInstantly,
                        const PopupPosition position,
                        const PopupToolBehavior override )
  {
      
    //if is gesture controlled, we do not want to add tooltips to objects
    InterSpecApp *app = dynamic_cast<InterSpecApp *>( wApp );
    if (app && app->isMobile())
       return;
    
#if( USE_OSX_NATIVE_MENU )
    PopupDivMenuItem *menuitem = dynamic_cast<PopupDivMenuItem *>( widget );
    if( menuitem && menuitem->getNsMenuItem() )
    {
      addOsxMenuItemToolTip( menuitem->getNsMenuItem(), text.c_str() );
      return;
    }//if( menuitem )
#endif
    
    //get the InterSpecApp so we can access the value set (from the preferences) whether
    //we want to display the tooltips immediately for beginners.  Pro users also hide
    //the tooltip when they start typing.
    
    string delay ="1000";
    string keyPressHide="";

    const bool overrideShow = (override==AlwaysShow);
    
    if( overrideShow || showInstantly )
      delay = "0";  //for beginners, so the tooltip shows immediately, instead of default 1 second
    else
      keyPressHide= "keypress"; //for pro users so the tooltip hides
    
    //Create popup notifications
    std::stringstream strm;
    
    //need to escape the ' in the text message
    std::string val = text;
    boost::replace_all(val, "'", "\\'");
    
    string pos = "";
    
    switch(position) {
      case Top: pos = "my: 'bottom center', at: 'top center', "; break;
      case Bottom: pos = "my: 'top center', at: 'bottom center', ";break;
      case Right: pos = "my: 'left center', at: 'right center', ";break;
      case Left: pos = "my: 'right center', at: 'left center', ";break;
    }
    //Note: need to prerender, as toggling requires tooltip already rendered.
    strm << "$('#"<<widget->id()<<"').qtip({ "
        "prerender: true, "
        "content:  { text: '"<< val <<"'}, "
        "position: { " << pos <<
                    "viewport: $(window), "
                    "adjust: { "
                        "method: 'flipinvert flipinvert', "
                        "x:5} "
                    "},"
        "show:  {  event: 'mouseenter focus', "
                  "delay: "<< delay << ""
              "},"
        "hide:  {  fixed: true, event: 'mouseleave focusout "<< keyPressHide <<"'  },"
        "style: { classes: 'qtip-rounded qtip-shadow" << (overrideShow ? "" : " delayable") << "',"
                  "tip: {"
                        "corner: true, "
                        "width: 18, "
                        "height: 12}"
                "}"
        "});";
    widget->doJavaScript(strm.str());
    
  }//  void attachToolTipOn( Wt::WWebWidget* widget, const std::string &text, bool force)

} //namespace HelpSystem