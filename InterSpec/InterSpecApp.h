#ifndef InterSpecApp_h
#define InterSpecApp_h
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

#include <set>
#include <mutex>
#include <string>

#include <boost/date_time.hpp>

#include <Wt/WApplication>
#include <Wt/WContainerWidget>


class InterSpec;
namespace Wt
{
  class WText;
  class WGridLayout;
  class WPushButton;
}//namespace Wt

class PopupDivMenuItem;

#if( INCLUDE_ANALYSIS_TEST_SUITE )
class SpectrumViewerTester;
#endif

//Feature under development (20171203) to prompt user if they want to load their
//  previous state, before actually doing it.
// Could make this always true for anything but mobile devices...
// Still deciding if I like this or not... laoding the state doesnt seem to be
//   a huge slowdown in the startup.
#if( BUILD_AS_ELECTRON_APP || BUILD_AS_OSX_APP )
#define PROMPT_USER_BEFORE_LOADING_PREVIOUS_STATE 0
#else
#define PROMPT_USER_BEFORE_LOADING_PREVIOUS_STATE 0
#endif

class InterSpecApp : public Wt::WApplication
{
public:
  InterSpecApp( const Wt::WEnvironment& env );
  virtual ~InterSpecApp();

  //isMobile(): Tests to see if a tablet or a phone platform
  bool isMobile() const;
  
  //isPhone(): Tests to see if on a iPhone, Android phone, or Blackberry device
  bool isPhone() const;
  
  //isTablet(): Tests to see if on a iPad or Android tablet
  bool isTablet() const;
  
  //isAndroid(): Searches for Android in the user agent string
  bool isAndroid() const;
  
#if( WT_VERSION>=0x3030400 )
  //ToDo: define a custom handler for javascript errors
//  virtual void handleJavaScriptError(const std::string& errorText)
//  {
//    std::cerr << "JS error: " << errorText << std::endl;
//  }
#endif
  
  /* svlog send the message to the InterSpec for display to the user. */
  void svlog( const Wt::WString& message,
              const Wt::WString& source,
              int priority = 1 );

  InterSpec *viewer();

  
  //userNameFromOS(): Caution, will return 'apache' if being served, from
  //  an apache server, 'mobile' if on a iOS device, or blank upon failure.
  static std::string userNameFromOS();

  //These functions functions should be moved to a differenct header/source file
  //  perhaps named ServerUtilityFunctions.h/.cpp
  std::string getUserNameFromEnvironment() const;  //returns CGI env variable "REMOTE_USER"
  
  //tempDirectory(): when the app is deployed as a FCGI application, the temp
  //  path may not be avaliable as a system environment variable, or if it is,
  //  it might not be the same as where Apache is configured to use.
  //  The answer returned by this function comes from first checking the CGI
  //  environemnt parameters, and then falling back to
  //  UtilityFunctions::temp_dir() if CGI doesnt specify temp directory
  //  locations.
  //  The actual answer returned is actually the result cached from the first
  //  call to this function where WApplication::instance() was non-null; the
  //  InterSpecApp constructor calls this function so the proper directory
  //  can be resolved at any future point.  Calls before a InterSpecApp
  //  is created may return just the result of UtilityFunctions::temp_dir().
  //This function is necassarry as essentually a workaround for FCGI deployment.
  //Note that currently this function requires locking a mutex, as well as some
  //  other not-super-fast actions.
  static std::string tempDirectory();

  
#if( !BUILD_FOR_WEB_DEPLOYMENT )
  std::string sessionUrlId();
  
  //
  bool userOpenFromFileSystem( const std::string &path );

#if( ALLOW_URL_TO_FILESYSTEM_MAP )
  bool openFileFromDbFileSystemLink( int index );
#endif
  
  static std::set<InterSpecApp *> runningInstances();
  static InterSpecApp *instanceFromExtenalIdString( const std::string &idstr );
  
  //Need put put in a function that will minimize memmorry usage of all cuurently
  //  running sessions.
#endif

#if( defined(WIN32) && BUILD_AS_ELECTRON_APP )
  //When users drag files from Outlook on windows into the app
  //  you can call the following functions
  void dragEventWithFileContentsStarted();
  void dragEventWithFileContentsFinished();
#endif
  
#if( BUILD_AS_ELECTRON_APP )
  static bool isElectronInstance();
#endif

#if( BUILD_AS_OSX_APP )
  static void osThemeChange( std::string name );
#endif
  
protected: 

  //notify(): over-riding WApplication::notify inorder to catch any exceptions
  //  that may happen during event handinling
  virtual void notify( const Wt::WEvent &event );

  //finalize(): called before destruction to take care of things that might
  //  involove calls to virtial functions.  We use it to record usage statistics
  virtual void finalize();
  
  //unload(): called when user navigates away from the page (or reloads)
  virtual void unload();
  
  //prepareForEndOfSession(): increments users total time in app and saves the
  //  state if their preferences ask for it
  virtual void prepareForEndOfSession();
  
  //clearSession(): resets the session to a blank session (e.g. doesnt load
  //  any saved state).
  virtual void clearSession();
  
  //setupDomEnvironment(): loads required JS and CSS resource, sets the
  //  appropriate meta tags.
  void setupDomEnvironment();
  
  //setupWidgets(): initializes the InterSpec widget, as well as the header
  //  and footer if applicable; if app has already been initialized, the
  //  existing widgets will be deleted and then re-created.
  //  If 'attemptStateLoad' is specified, the app will attempt to load the most
  //  recent saved state, unless the URL contains a restore=0 argument.
  void setupWidgets( const bool attemptStateLoad );
  
  
protected:
  InterSpec *m_viewer;
  Wt::WGridLayout *m_layout;

#if( !BUILD_FOR_WEB_DEPLOYMENT )
  Wt::Signal<const InterSpecApp *> m_destructing;
#endif
  
  boost::posix_time::ptime m_lastAccessTime;
  boost::posix_time::time_duration m_activeTimeInSession;
  
#define OPTIMISTICALLY_SAVE_USER_STATE 0
  //If OPTIMISTICALLY_SAVE_USER_STATE is enabled, then the users state will
  //  attempt to be saved whenever a 'onbeforeunload' is recieved.  The downside
  //  is that right now for chrome this doesnt work for a simple refresh, and
  //  it may be different for different browsers.
  //For the case when the server doesnt get the signal, and the user has
  //  navigated away, we could decrease the timeout settings in wt_config.xml
  //  so the session gets destroyed quicker after refreshes
  
#if( OPTIMISTICALLY_SAVE_USER_STATE )
  //m_thinkingLeaveSignal: an experimental signal that tries to catch unload
  //  events a little better; still doesnt catch refresh events.
  std::unique_ptr<Wt::JSignal<> > m_thinkingLeaveSignal;
#endif
  
  std::unique_ptr<Wt::JSignal<> > m_clearSession;
  
#if( INCLUDE_ANALYSIS_TEST_SUITE )
  friend class SpectrumViewerTester;
#endif
};//class InterSpecApp

#endif  //InterSpecApp_h

