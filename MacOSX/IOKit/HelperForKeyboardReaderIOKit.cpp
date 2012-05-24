

#include "HelperForKeyboardReaderIOKit.h"

#define wxLogDebug(...)

#include <boost/format.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <mach/mach_error.h>

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDKeys.h>



namespace
{
    void LogErrorWhenFunctorIsntEmpty
    (
     boost::function< void ( const std::string msg ) > errorLoggerFunctor,
     const std::string error
    )
    {
        if( errorLoggerFunctor.empty() == false )
        {
            errorLoggerFunctor( error );
        }
    }

    std::string CF2StdString ( CFStringRef cf_str )
    {
        std::string result;

        if (cf_str)
        {
            static const CFStringEncoding encoding = kCFStringEncodingUTF8;
            const CFIndex max_utf8_str_len = CFStringGetMaximumSizeForEncoding
                ( CFStringGetLength (cf_str), encoding );

            if ( max_utf8_str_len > 0 )
            {
                result.resize(max_utf8_str_len);

                if (CFStringGetCString (cf_str, &result[0], result.size(), encoding))
                {
                    result.resize(strlen(result.c_str()));
                }
            }
        }

        return result;
    }

}



/// a simple 'tuple' that contains the following items corresponding to ONE KEY on the keyboard:
///     a human-readable name, a usage-id, the mac cookie, and an ignore/utilize app-specific preference
struct GitHubSample::HelperForKeyboardReaderIOKit::PerKeyData
{
    PerKeyData( const std::string& keyName,
                const unsigned int usageId,
                const IOHIDElementCookie cookie,
                const bool ignore = false )
        : name( keyName ),
          usbOfficialUsageID( usageId ),
          macCookieValue( cookie ),
          mustBeIgnoredByOurApplication( ignore )
    {}

    std::string name;
    unsigned int usbOfficialUsageID;
    IOHIDElementCookie macCookieValue;
    bool mustBeIgnoredByOurApplication;

    /// simple helper function used for the initial population of a vector of PerKeyData structs
    static void PushOneItem
    (
     std::vector< boost::shared_ptr<GitHubSample::HelperForKeyboardReaderIOKit::PerKeyData> >& keysVector,
     const std::string& keyName,
     const unsigned int usageId,
     const IOHIDElementCookie cookie,
     const bool ignore = false
    )
    {
        boost::shared_ptr< GitHubSample::HelperForKeyboardReaderIOKit::PerKeyData > ptr
            ( new GitHubSample::HelperForKeyboardReaderIOKit::PerKeyData( keyName, usageId, cookie, ignore ) );
        keysVector.push_back( ptr );
    }
};


/// Using the pimpl idiom so that IOKit headers don't have to be 'pound-included' in 'HelperForKeyboardReaderIOKit.h'
struct GitHubSample::HelperForKeyboardReaderIOKit::PrivateImpl
{
    io_object_t            m_hidDevice;
    IOHIDDeviceInterface** m_hidDeviceInterface;
    IOCFPlugInInterface**  m_plugInInterface;
    IOHIDQueueInterface**  m_hidQueue;

    PrivateImpl()
        : m_hidDevice( (io_object_t)0 ),
          m_hidDeviceInterface(NULL),
          m_plugInInterface(NULL),
          m_hidQueue(NULL)
    {}

    ~PrivateImpl();
};


GitHubSample::HelperForKeyboardReaderIOKit::PrivateImpl::~PrivateImpl()
{
    /*
      more from Apple:

      If you suspect that you are leaking io_object_t objects, however,
      IOObjectGetRetainCount won't help you because this function informs you of
      the underlying kernel object's retain count (which is often much
      higher). Instead, because the retain count of an io_object_t object is
      essentially the retain count of the send rights on the Mach port, you use
      a Mach function to get this information. Listing 4-3 shows how to use the
      Mach functionmach_port_get_refs (defined in mach/mach_port.h in the Kernel
      framework) to get the retain count of an io_object_t object.

      #include <mach/mach_port.h>

      kern_return_t kr;
      unsigned int count;
      io_object_t theObject;

      kr = mach_port_get_refs ( mach_task_self(), theObject, MACH_PORT_RIGHT_SEND,  &count );

      printf ("Retain count for object ID %#X is %d\n", theObject, count);
     */

    if ( m_hidDeviceInterface )
    {
        (void)(*m_hidDeviceInterface)->close(m_hidDeviceInterface);
        (void)(*m_hidDeviceInterface)->Release(m_hidDeviceInterface);
    }

    if ( m_plugInInterface )
    {
        IODestroyPlugInInterface( m_plugInInterface );
    }

    if ( m_hidQueue )
    {
        (void)(*m_hidQueue)->Release(m_hidQueue);
    }

    if ( m_hidDevice )
    {
        IOObjectRelease( m_hidDevice );
    }
}


GitHubSample::HelperForKeyboardReaderIOKit::HelperForKeyboardReaderIOKit
(
 const bool enableQueue,
 boost::function< void ( const std::string msg ) > errorLoggerFunctor
)
    : m_pimpl( new PrivateImpl ),
      m_errorLoggerFunctor( errorLoggerFunctor ),
      m_queueEnabled( enableQueue )
{
    Initialize();
}


void GitHubSample::HelperForKeyboardReaderIOKit::Initialize()
{
    bool basicSuccess = false;

    if ( FindKeyboard()
         && CreatePluginInterface()
         && CreateDeviceInterface()
         && PopulateVectorOfKeyInfo()
         && FindKeypressCookies()

    )
    {
        basicSuccess = true;
        wxLogDebug( wxT("HelperForKeyboardReaderIOKit::Initialize -- basic systems go!") );
    }
    else
    {
        LogInitializationError( "Failed basic keyboard initialization." );
        m_pimpl.reset();
    }

    if ( basicSuccess && m_queueEnabled )
    {
        if ( CreateQueue()
             && AddElementsToQueue() )
        {
            wxLogDebug( wxT("HelperForKeyboardReaderIOKit::Initialize -- all systems go!") );
        }
        else
        {
            LogInitializationError( "Failed basic keyboard input queue initialization." );
        }
    }
}


void GitHubSample::HelperForKeyboardReaderIOKit::LogInitializationError( const std::string& errorDesc ) const
{
    std::string keyboardInfo;
    std::vector< std::string >::const_iterator iter = m_deviceInformationProperties.begin();
    while ( iter != m_deviceInformationProperties.end() )
    {
        keyboardInfo += (*iter);
        keyboardInfo += "\n";
        iter++;
    }

    if ( m_deviceInformationProperties.empty() )
    {
        keyboardInfo = errorDesc;
    }
    else
    {
        keyboardInfo = errorDesc + " Keyboard description follows:\n" + keyboardInfo;
    }

    LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, keyboardInfo );
}

// Note: we return a NEGATIVE value to indicate error.
int GitHubSample::HelperForKeyboardReaderIOKit::CountOfCurrentlyDepressedKeys() const
{
    if ( ! m_pimpl )
    {
        return -1; // BAILING OUT EARLY!!  BAILING OUT EARLY!!  BAILING OUT EARLY!!
    }

    DebugCheckErrorKeys();

    int score = 0;
    std::vector< boost::shared_ptr<PerKeyData> >::const_iterator iter = m_keys.begin();
    while( iter != m_keys.end() )
    {
        if( (*iter)->macCookieValue != 0 && (*iter)->mustBeIgnoredByOurApplication == false )
        {
            IOHIDEventStruct theEvent;

            IOReturn ioReturnValue = (*m_pimpl->m_hidDeviceInterface)->getElementValue
                (m_pimpl->m_hidDeviceInterface,
                 (*iter)->macCookieValue,
                 &theEvent);

            if (ioReturnValue != kIOReturnSuccess)
            {
                assert( ! "failed to get element value." );
            }

            wxLogDebug( wxT("event value is %Ld") , static_cast<long long int>( theEvent.value ) );

            if( theEvent.value != 0 )
            {
                score++;
            }
        }

        iter++;
    }

    return score;
}


void GitHubSample::HelperForKeyboardReaderIOKit::DebugCheckErrorKeys() const
{
#ifdef _DEBUG

    IOHIDEventStruct theEvent;
    IOReturn ioReturnValue = (*m_pimpl->m_hidDeviceInterface)->getElementValue
        (m_pimpl->m_hidDeviceInterface,
         (m_keys[kHIDUsage_KeyboardErrorRollOver])->macCookieValue,
         &theEvent);

    if (ioReturnValue == kIOReturnSuccess)
    {
        if( theEvent.value != 0 )
        {
            LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "kHIDUsage_KeyboardErrorRollOver" );
        }
    }

    ioReturnValue = (*m_pimpl->m_hidDeviceInterface)->getElementValue
        (m_pimpl->m_hidDeviceInterface,
         (m_keys[kHIDUsage_KeyboardPOSTFail])->macCookieValue,
         &theEvent);

    if (ioReturnValue == kIOReturnSuccess)
    {
        if( theEvent.value != 0 )
        {
            LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "kHIDUsage_KeyboardPOSTFail" );
        }
    }

    ioReturnValue = (*m_pimpl->m_hidDeviceInterface)->getElementValue
        (m_pimpl->m_hidDeviceInterface,
         (m_keys[kHIDUsage_KeyboardErrorUndefined])->macCookieValue,
         &theEvent);

    if (ioReturnValue == kIOReturnSuccess)
    {
        if( theEvent.value != 0 )
        {
            LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "kHIDUsage_KeyboardErrorUndefined" );
        }
    }

    ioReturnValue = (*m_pimpl->m_hidDeviceInterface)->getElementValue
        (m_pimpl->m_hidDeviceInterface,
         (m_keys[kHIDUsage_KeyboardPower])->macCookieValue,
         &theEvent);

    if (ioReturnValue == kIOReturnSuccess)
    {
        if( theEvent.value != 0 )
        {
            LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "kHIDUsage_KeyboardPower" );
        }
    }

#endif // #ifdef _DEBUG
}

/*
  The code inside this function works well.  The reason for tagging this as an
  'experimental' function is that there are a number of things that need to be
  decided before this is a 'product-ready' function.  For one thing, it almost
  certainly needs to RETURN SOMETHING.  If it will return some object or value
  to represent one event, then we may need to change the while loop so that we
  only ever PULL ONE EVENT at a time.  Currently we can retrieve many events
  during one call to this function, so which one would you return? The latest?
  The earliest? All of them?
 */
void GitHubSample::HelperForKeyboardReaderIOKit::ReadFromQueue_Experimental()
{
    if ( (! m_pimpl) || (! m_pimpl->m_hidQueue) )
    {
        return; // BAILING OUT EARLY!!  BAILING OUT EARLY!!  BAILING OUT EARLY!!
    }


    IOReturn ioReturnValue = kIOReturnError;
    AbsoluteTime zeroTime = {0, 0};
    IOHIDEventStruct the_event;

    while( kIOReturnSuccess ==
           (ioReturnValue = (*(IOHIDQueueInterface**) m_pimpl->m_hidQueue)->
            getNextEvent( m_pimpl->m_hidQueue,
                          &the_event,
                          zeroTime,
                          0
                          ))
           )
    {
    /*
            enum IOHIDElementType {
            kIOHIDElementTypeInput_Misc        = 1,
            kIOHIDElementTypeInput_Button      = 2,
            kIOHIDElementTypeInput_Axis        = 3,
            kIOHIDElementTypeInput_ScanCodes   = 4,
            kIOHIDElementTypeOutput            = 129,
            kIOHIDElementTypeFeature           = 257,
            kIOHIDElementTypeCollection        = 513
            };

            struct IOHIDEventStruct
            {
                IOHIDElementType    type;
                IOHIDElementCookie  elementCookie;
                int32_t             value;
                AbsoluteTime        timestamp;
                uint32_t            longValueSize;
                void *              longValue;
            };
     */

        // they seem to all be of type kIOHIDElementTypeInput_Button
        if ( the_event.type != kIOHIDElementTypeInput_Button )
        {
            wxLogDebug( wxT("the keyboard sent some event that was not of the button type??") );
        }
        else
        {
            std::string msg = boost::str( boost::format("event from queue. code: %1%. cookie: %2%. value %3%")
                                          % (int)the_event.type % (int)the_event.elementCookie % the_event.value );

            if( the_event.value )
            {
                msg = "KEY PRESS " + msg;
            }
            else
            {
                msg = "KEY RELEASE " + msg;
            }

            // TODO - we could use m_keys to locate the elementCookie and get the 'name' string from the PerKeyData struct
        }
    }

    if ( ioReturnValue != kIOReturnUnderrun )
    {
        if ( kIOReturnSuccess == ioReturnValue )
        {
            assert( ! "\n\nif we had gotten this, then we should have kept looping\n\n");
        }

        std::string msg = boost::str( boost::format("getNextEvent failed. code: %1%") % (int)ioReturnValue );
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, msg );
    }
}


/// Credit goes to Amit Singh.  http://osxbook.com/book/bonus/chapter10/kbdleds/
bool GitHubSample::HelperForKeyboardReaderIOKit::FindKeyboard()
{
    /*
      From Apple doc "The IOKitLib API":

      Because IOService is a subclass of IORegistryEntry, for example, you can
      use an io_service_t object with any IOKitLib function that expects an
      io_registry_entry_t object, such as IORegistryEntryGetPath.
     */
    io_service_t result = (io_service_t)0;

    /*
      A matching dictionary is a dictionary of key-value pairs that describe the
      properties of a device or other service. You create a matching dictionary
      to specify the types of devices your application needs to access. The I/O
      Kit provides several general keys you can use in your matching dictionary
      and many device families define specific keys and matching
      protocols. During device matching (described next) the values in a
      matching dictionary are compared against nub properties in the I/O
      Registry.

      Creating Matching Dictionaries

      When you use IOKitLib functions to create a matching dictionary, you
      receive a reference to a Core Foundation dictionary object. The IOKitLib
      uses Core Foundation classes, such as CFMutableDictionary and CFString,
      because they closely corrrespond to the in-kernel collection and container
      classes, such as OSDictionary and OSString (defined in libkern/c++ in the
      Kernel framework).

      The I/O Kit automatically translates a CFDictionary object into its
      in-kernel counterpart when it crosses the user-kernel boundary, allowing
      you to create an object in user-space that is later used in the
      kernel. For more information on using Core Foundation objects to represent
      in-kernel objects, see 'Viewing Properties of I/O Registry Objects.'

      These functions create a mutable Core Foundation dictionary object
      containing the appropriate key and your passed-in value.

      function: IOServiceMatching .  key: kIOProviderClassKey .  file: IOKitKeys.h

      All dictionary-creation functions return a reference to a
      CFMutableDictionary object. Usually, you pass the dictionary to one of the
      look-up functions (discussed next in 'Looking Up Devices'), each of which
      consumes one reference to it. If you use the dictionary in some other way,
      you should adjust its retain count accordingly, using CFRetain or
      CFRelease (defined in the Core Foundation framework).
     */
    CFMutableDictionaryRef matchingDictRef = (CFMutableDictionaryRef)0;

    if (!(matchingDictRef = IOServiceMatching(kIOHIDDeviceKey)))
    {
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "Failed to retrieve device key matching dictionary." );
        return false;
    }

    CFNumberRef usagePageRef = (CFNumberRef)0;
    CFNumberRef usageRef = (CFNumberRef)0;
    UInt32 usagePage = kHIDPage_GenericDesktop;
    UInt32 usage = kHIDUsage_GD_Keyboard;

    usagePageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usagePage);
    usageRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);

    if ( (!usagePageRef) || (!usageRef) )
    {
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "Failed to find kHIDPage_GenericDesktop and/or kHIDUsage_GD_Keyboard." );
    }
    else
    {
        CFDictionarySetValue(matchingDictRef, CFSTR(kIOHIDPrimaryUsagePageKey), usagePageRef);
        CFDictionarySetValue(matchingDictRef, CFSTR(kIOHIDPrimaryUsageKey), usageRef);

        /*
          If you receive an io_iterator_t object from IOServiceGetMatchingServices,
          you should release it with IOObjectRelease when you're finished with it;
          similarly, you should use IOObjectRelease to release the io_object_t
          object you receive from IOServiceGetMatchingService.

          Getting the I/O Kit Master Port

          When your application uses functions that communicate directly with
          objects in the kernel, such as objects that represent devices, it does so
          through a Mach port, namely, the I/O Kit master port. Several I/O Kit
          functions require you to pass in an argument identifying the port you're
          using. Starting with Mac OS X version 10.2, you can fulfill this
          requirement in either of two ways:

          You can get the I/O Kit master port from the function IOMasterPort and
          pass that port to the I/O Kit functions that require a port argument.

          You can pass the constant kIOMasterPortDefault to all I/O Kit functions
          that require a port argument.
        */
        result = IOServiceGetMatchingService(kIOMasterPortDefault, matchingDictRef);
    }

    if (usageRef)
    {
        CFRelease(usageRef);
    }
    if (usagePageRef)
    {
        CFRelease(usagePageRef);
    }

    m_pimpl->m_hidDevice = result;
    return (result != 0);
}


/// Credit goes to Amit Singh.  http://osxbook.com/book/bonus/chapter10/kbdleds/
/**
   What is the difference between a device and a device interface?

   Answer from docs on usb.org:

   A USB device may be a single class type or it may be composed of multiple
   classes. For example, a telephone hand set might use features of the HID,
   Audio, and Telephony classes. This is possible because the class is specified
   in the Interface descriptor and not the Device descriptor.
 */
bool GitHubSample::HelperForKeyboardReaderIOKit::CreatePluginInterface()
{
    /*
      From: https://developer.apple.com/library/mac/#documentation/devicedrivers/conceptual/IOKitFundamentals/Matching/Matching.html

      One common property of personalities is the probe score. A probe score is
      an integer that reflects how well-suited a driver is to drive a particular
      device. A driver may have an initial probe-score value in its personality
      and it may implement a probe function that allows it to modify this
      default value, based on its suitability to drive a device. As with other
      matching values, probe scores are specific to each family. That's because
      once matching proceeds past the class-matching stage, only personalities
      from the same family compete. For more information on probe scores and
      what a driver does in the probe function, see "Device Probing"
     */
    SInt32    probe_score = 0;
    IOReturn  ioReturnValue = kIOReturnError;

    ioReturnValue = IOCreatePlugInInterfaceForService
        ( m_pimpl->m_hidDevice,
          kIOHIDDeviceUserClientTypeID,
          kIOCFPlugInInterfaceID,
          &m_pimpl->m_plugInInterface,
          &probe_score // see comment block above about probe_score
        );

    if (ioReturnValue != kIOReturnSuccess)
    {
        std::string msg = boost::str( boost::format("IOCreatePlugInInterfaceForService failed with value %1%") % (int)ioReturnValue );
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, msg );
    }
    else
    {
        // we only use the keyboard properties as 'extra info', so we don't care
        // if getting properties succeeds or not.

        // Even though we use IORegistryEntryCreateCFProperty to get the
        // properties, and even though IORegistryEntryCreateCFProperty *only*
        // needs our 'io_object_t' (m_hidDevice), for some CRAZY reason we
        // cannot get any properties until our IOCFPlugInInterface
        // (m_plugInInterface) is created!
        GetKeyboardProperties();
    }

    return (ioReturnValue==kIOReturnSuccess);
}


/**
   We only use the properties as 'extra info', so we don't care if this fails.

   Also: Even though we use IORegistryEntryCreateCFProperty to get the
   properties, and even though IORegistryEntryCreateCFProperty *only* needs our
   'io_object_t' (m_hidDevice), for some CRAZY reason we cannot get any
   properties until our IOCFPlugInInterface (m_plugInInterface) is created!
*/
void GitHubSample::HelperForKeyboardReaderIOKit::GetKeyboardProperties()
{
    StoreOneProperty( CFSTR( kIOHIDTransportKey ) );
    StoreOneProperty( CFSTR( kIOHIDVendorIDKey ) );
    StoreOneProperty( CFSTR( kIOHIDVendorIDSourceKey ) );
    StoreOneProperty( CFSTR( kIOHIDProductIDKey ) );
    StoreOneProperty( CFSTR( kIOHIDVersionNumberKey ) );
    StoreOneProperty( CFSTR( kIOHIDManufacturerKey ) );
    StoreOneProperty( CFSTR( kIOHIDProductKey ) );
    StoreOneProperty( CFSTR( kIOHIDSerialNumberKey ) );
    StoreOneProperty( CFSTR( kIOHIDCountryCodeKey ) );
    StoreOneProperty( CFSTR( kIOHIDLocationIDKey ) );
}


void GitHubSample::HelperForKeyboardReaderIOKit::StoreOneProperty
(
 CFStringRef propertyKey
)
{
    CFTypeRef propertyValueString;

    propertyValueString = IORegistryEntryCreateCFProperty
        ( m_pimpl->m_hidDevice,
          propertyKey,
          kCFAllocatorDefault,0);

    if (  !  propertyValueString)
    {
        wxLogDebug( wxT("didn't get property") );
    }
    else
    {
        std::string property = CF2StdString( propertyKey );
        std::string asStdString = "<unknown>";

        if ( CFGetTypeID(propertyValueString) == CFNumberGetTypeID() )
        {
            long value = 0;
            CFNumberGetValue((CFNumberRef) propertyValueString, kCFNumberLongType, &value);
            asStdString = boost::str( boost::format("%1%") % value );
        }
        else if ( CFGetTypeID(propertyValueString) == CFStringGetTypeID() )
        {
            asStdString = CF2StdString( (CFStringRef) propertyValueString );
        }
        else
        {
            asStdString = "<type error>";
        }

        m_deviceInformationProperties.push_back( property + ": " + asStdString );
    }

    if(propertyValueString)
    {
        CFRelease(propertyValueString);
    }
}



bool GitHubSample::HelperForKeyboardReaderIOKit::CreateDeviceInterface()
{
    /*
      After your application gets the IOCFPlugInInterface object, it then calls
      its QueryInterface function, supplying it with (among other arguments) the
      family-defined UUID name of the particular device interface the
      application needs. The QueryInterface function returns an instance of the
      requested device interface and the application then has access to all the
      functions the device interface provides.

      When you use a device interface to communicate with a device, a user
      client object joins the driver stack. A family that provides a device
      interface also provides the user client object that transmits an
      application's commands from the device interface to the device. When your
      application requests a device interface for a particular device, the
      device's family instantiates the appropriate user client object, typically
      attaching it in the I/O Registry as a client of the device nub.
     */
    HRESULT plugInResult = (*m_pimpl->m_plugInInterface)->QueryInterface
        ( m_pimpl->m_plugInInterface,
          CFUUIDGetUUIDBytes(kIOHIDDeviceInterfaceID),
          (LPVOID *)&m_pimpl->m_hidDeviceInterface);

    bool returnValue = false;

    if( plugInResult != S_OK )
    {
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "Failed to create IOHIDDeviceInterface." );
    }
    else
    {
        IOReturn ioReturnValue = (*m_pimpl->m_hidDeviceInterface)->open(m_pimpl->m_hidDeviceInterface, 0);
        if (ioReturnValue != kIOReturnSuccess)
        {
            std::string msg = boost::str( boost::format("Failed to open the IOHIDDeviceInterface. Failed with value %1%") % (int)ioReturnValue );
            LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, msg );
        }
        else
        {
            returnValue = true;
        }
    }

    return returnValue;
}


/**
   We are doing a NON-recursive search for cookies.

   In VirtualBox, a *RECURSIVE* search is done to find the cookies for the
   modifier keys.  Their function is 'darwinBruteForcePropertySearch' and it
   calls itself recursively for each item in the dictionary that is a non-leaf
   item (meaning the item is also a dictionary -- a dictionary within a
   dictionary).

   (as of May 22, 2012)
   http://www.virtualbox.org/svn/vbox/trunk/src/VBox/Frontends/VirtualBox/src/platform/darwin/DarwinKeyboard.cpp

 */
bool GitHubSample::HelperForKeyboardReaderIOKit::FindKeypressCookies()
{
    CFArrayRef         elements;
    IOReturn           ioReturnValue = kIOReturnError;

    ioReturnValue = (*(IOHIDDeviceInterface122 **)m_pimpl->m_hidDeviceInterface)->copyMatchingElements
        ( m_pimpl->m_hidDeviceInterface,
          NULL,
          &elements );

    if (ioReturnValue != kIOReturnSuccess)
    {
        std::string msg = boost::str( boost::format("copyMatchingElements failed. code: %1%") % (int)ioReturnValue );
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, msg );
    }
    else
    {
        for (CFIndex i = 0; i < CFArrayGetCount(elements); i++)
        {
            // these THREE variables are 'helpers' ....
            CFDictionaryRef    element;
            CFTypeRef          temp_object_reused;
            long               temp_number_reused;

            // these three variables are what we REALLY ACTUALLY NEED TO discover!
            IOHIDElementCookie cookie;
            long               usage;
            long               usagePage;

            element = (CFDictionaryRef) CFArrayGetValueAtIndex(elements, i);

            temp_object_reused = (CFDictionaryGetValue(element, CFSTR(kIOHIDElementCookieKey)));

            if ( temp_object_reused == 0
                 || (CFGetTypeID(temp_object_reused) != CFNumberGetTypeID())
                 || (!CFNumberGetValue((CFNumberRef) temp_object_reused, kCFNumberLongType, &temp_number_reused))
                 )
            {
                wxLogDebug( wxT("no cookie key here") );
                continue;
            }

            // Yay! After all that conversion crud, we now have the cookie!
            cookie = (IOHIDElementCookie)temp_number_reused;

            temp_object_reused = CFDictionaryGetValue(element, CFSTR(kIOHIDElementUsageKey));

            if ( temp_object_reused == 0
                 || (CFGetTypeID(temp_object_reused) != CFNumberGetTypeID())
                 || (!CFNumberGetValue((CFNumberRef)temp_object_reused, kCFNumberLongType, &temp_number_reused))
                 )
            {
                LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "A cookie without a usage id?" );
                continue;
            }

            // Yay! After all that conversion crud, we now have the usage id!
            usage = temp_number_reused;

            temp_object_reused = CFDictionaryGetValue(element,CFSTR(kIOHIDElementUsagePageKey));

            if ( temp_object_reused == 0
                 || (CFGetTypeID(temp_object_reused) != CFNumberGetTypeID())
                 || (!CFNumberGetValue((CFNumberRef)temp_object_reused, kCFNumberLongType, &temp_number_reused))
                 )
            {
                LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "A cookie without a usage page?" );
                continue;
            }

            // Yay! After all that conversion crud, we now have the usage id!
            usagePage = temp_number_reused;

            if (usagePage == kHIDPage_KeyboardOrKeypad)
            {
                if ( usage >= static_cast<long>(m_keys.size()) )
                {
                    // there are quite a few that are higher than we care about.
                    //wxLogDebug(wxT("some usage key of a higher value than we care about was found.") );
                }
                else if ( usage < 0 )
                {
                    wxLogDebug( wxT("A negative valued usage id?") );
                }
                else
                {
                    if( m_keys[ usage ]->macCookieValue != 0 )
                    {
                        // I have so far never seen this happen...
                        assert( ! "we found the same usage key twice (or more) ?" );
                    }
                    else
                    {
                        m_keys[ usage ]->macCookieValue = cookie;
                    }
                }
            }
        }
    }

    int score = 0;
    std::vector< boost::shared_ptr<PerKeyData> >::const_iterator iter = m_keys.begin();
    while( iter != m_keys.end() )
    {
        if( (*iter)->macCookieValue != 0 && (*iter)->mustBeIgnoredByOurApplication == false )
        {
            score++;
            wxLogDebug( wxT("located cookie for:\t%s"), (*iter)->name.c_str() );
        }

        iter++;
    }

    wxLogDebug( wxT("our vector size is %d and the score is %d"), m_keys.size(), score );

    if(elements)
    {
        CFRelease(elements);
    }

    return (score > 40);// if we don't find at least 40 cookies, we consider our search to have FAILED
}


bool GitHubSample::HelperForKeyboardReaderIOKit::CreateQueue()
{
    bool success = false;
    IOReturn  ioReturnValue = kIOReturnError;

    m_pimpl->m_hidQueue = (*(IOHIDDeviceInterface**) m_pimpl->m_hidDeviceInterface)
        ->allocQueue (m_pimpl->m_hidDeviceInterface);

    if (  ! (m_pimpl->m_hidQueue))
    {
        LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "Failed to alloc IOHIDQueueInterface ** via allocQueue" );
    }
    else
    {
        ioReturnValue = (*(IOHIDQueueInterface**) m_pimpl->m_hidQueue)->create
            ( m_pimpl->m_hidQueue,
              /* passing 1 for the second argument got MORE EVENTS than with 0,
                 but i still had to do the cookie-checking stuff.

                 1 kIOHIDQueueOptionsTypeEnqueueAll: Pass
                 kIOHIDQueueOptionsTypeEnqueueAll option to force the IOHIDQueue
                 to enqueue all events, relative or absolute, regardless of
                 change.
              */
              0, // when i use zero, i appear to ONLY get what matches my cookies. however, to use 1, you apparently need to set at least 1 cookie still, but then you get EVERYTHING.
              200  // The maximum number of elements in the queue before the oldest elements in the queue begin to be lost.
            );

        if (kIOReturnSuccess != ioReturnValue)
        {
            std::string msg = boost::str( boost::format("Failed to create queue. Error: %1%") % (int)ioReturnValue );
            LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, msg );
        }
        else
        {
            // Start the queue...

            ioReturnValue = (*(IOHIDQueueInterface**) m_pimpl->m_hidQueue)->start(m_pimpl->m_hidQueue);

            if (ioReturnValue != kIOReturnSuccess)
            {
                // got this one time: kIOReturnNotOpen
                LogErrorWhenFunctorIsntEmpty( m_errorLoggerFunctor, "Failed to start queue." );
            }
            else
            {
                success = true;
            }
        }
    }

    return success;
}


bool GitHubSample::HelperForKeyboardReaderIOKit::AddElementsToQueue()
{
    bool success = true;

    std::vector< boost::shared_ptr<PerKeyData> >::const_iterator iter = m_keys.begin();
    while( iter != m_keys.end() )
    {
        if( (*iter)->macCookieValue != 0 && (*iter)->mustBeIgnoredByOurApplication == false )
        {
            IOReturn ioReturnValue = (*(IOHIDQueueInterface**) m_pimpl->m_hidQueue)->addElement
                (m_pimpl->m_hidQueue, (*iter)->macCookieValue , 0);

            if (ioReturnValue != kIOReturnSuccess)
            {
                assert( ! "failed to add element to the queue" );
                success = false;
            }
        }

        iter++;
    }

    return success;
}








bool GitHubSample::HelperForKeyboardReaderIOKit::PopulateVectorOfKeyInfo()
{
    if ( false == m_keys.empty() )
    {
        assert( ! "\n\n you should only ever call this function ONCE, and that is when we do initial population\n\n" );
        return false; // BAILING OUT HERE!!  BAILING OUT HERE!!  BAILING OUT HERE!!
    }

    static const bool FORCE_APPLICATION_TO_IGNORE_THIS_KEY = true;// this is passed to some PerKeyData structs

    m_keys.reserve(250);

    PerKeyData::PushOneItem( m_keys, "BOGUS PLACEHOLDER AT INDEX ZERO", 0, 0 );

    // this stuff was all automatically generated. I did not sit here and type this out!  :)
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardErrorRollOver", kHIDUsage_KeyboardErrorRollOver, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPOSTFail", kHIDUsage_KeyboardPOSTFail, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardErrorUndefined", kHIDUsage_KeyboardErrorUndefined, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardA", kHIDUsage_KeyboardA, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardB", kHIDUsage_KeyboardB, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardC", kHIDUsage_KeyboardC, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardD", kHIDUsage_KeyboardD, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardE", kHIDUsage_KeyboardE, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF", kHIDUsage_KeyboardF, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardG", kHIDUsage_KeyboardG, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardH", kHIDUsage_KeyboardH, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardI", kHIDUsage_KeyboardI, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardJ", kHIDUsage_KeyboardJ, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardK", kHIDUsage_KeyboardK, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardL", kHIDUsage_KeyboardL, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardM", kHIDUsage_KeyboardM, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardN", kHIDUsage_KeyboardN, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardO", kHIDUsage_KeyboardO, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardP", kHIDUsage_KeyboardP, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardQ", kHIDUsage_KeyboardQ, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardR", kHIDUsage_KeyboardR, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardS", kHIDUsage_KeyboardS, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardT", kHIDUsage_KeyboardT, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardU", kHIDUsage_KeyboardU, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardV", kHIDUsage_KeyboardV, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardW", kHIDUsage_KeyboardW, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardX", kHIDUsage_KeyboardX, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardY", kHIDUsage_KeyboardY, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardZ", kHIDUsage_KeyboardZ, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard1", kHIDUsage_Keyboard1, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard2", kHIDUsage_Keyboard2, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard3", kHIDUsage_Keyboard3, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard4", kHIDUsage_Keyboard4, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard5", kHIDUsage_Keyboard5, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard6", kHIDUsage_Keyboard6, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard7", kHIDUsage_Keyboard7, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard8", kHIDUsage_Keyboard8, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard9", kHIDUsage_Keyboard9, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keyboard0", kHIDUsage_Keyboard0, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardReturnOrEnter", kHIDUsage_KeyboardReturnOrEnter, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardEscape", kHIDUsage_KeyboardEscape, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardDeleteOrBackspace", kHIDUsage_KeyboardDeleteOrBackspace, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardTab", kHIDUsage_KeyboardTab, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardSpacebar", kHIDUsage_KeyboardSpacebar, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardHyphen", kHIDUsage_KeyboardHyphen, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardEqualSign", kHIDUsage_KeyboardEqualSign, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardOpenBracket", kHIDUsage_KeyboardOpenBracket, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardCloseBracket", kHIDUsage_KeyboardCloseBracket, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardBackslash", kHIDUsage_KeyboardBackslash, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardNonUSPound", kHIDUsage_KeyboardNonUSPound, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardSemicolon", kHIDUsage_KeyboardSemicolon, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardQuote", kHIDUsage_KeyboardQuote, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardGraveAccentAndTilde", kHIDUsage_KeyboardGraveAccentAndTilde, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardComma", kHIDUsage_KeyboardComma, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPeriod", kHIDUsage_KeyboardPeriod, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardSlash", kHIDUsage_KeyboardSlash, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardCapsLock", kHIDUsage_KeyboardCapsLock, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF1", kHIDUsage_KeyboardF1, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF2", kHIDUsage_KeyboardF2, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF3", kHIDUsage_KeyboardF3, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF4", kHIDUsage_KeyboardF4, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF5", kHIDUsage_KeyboardF5, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF6", kHIDUsage_KeyboardF6, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF7", kHIDUsage_KeyboardF7, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF8", kHIDUsage_KeyboardF8, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF9", kHIDUsage_KeyboardF9, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF10", kHIDUsage_KeyboardF10, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF11", kHIDUsage_KeyboardF11, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF12", kHIDUsage_KeyboardF12, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPrintScreen", kHIDUsage_KeyboardPrintScreen, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardScrollLock", kHIDUsage_KeyboardScrollLock, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPause", kHIDUsage_KeyboardPause, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInsert", kHIDUsage_KeyboardInsert, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardHome", kHIDUsage_KeyboardHome, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPageUp", kHIDUsage_KeyboardPageUp, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardDeleteForward", kHIDUsage_KeyboardDeleteForward, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardEnd", kHIDUsage_KeyboardEnd, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPageDown", kHIDUsage_KeyboardPageDown, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardRightArrow", kHIDUsage_KeyboardRightArrow, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLeftArrow", kHIDUsage_KeyboardLeftArrow, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardDownArrow", kHIDUsage_KeyboardDownArrow, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardUpArrow", kHIDUsage_KeyboardUpArrow, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadNumLock", kHIDUsage_KeypadNumLock, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadSlash", kHIDUsage_KeypadSlash, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadAsterisk", kHIDUsage_KeypadAsterisk, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadHyphen", kHIDUsage_KeypadHyphen, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadPlus", kHIDUsage_KeypadPlus, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadEnter", kHIDUsage_KeypadEnter, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad1", kHIDUsage_Keypad1, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad2", kHIDUsage_Keypad2, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad3", kHIDUsage_Keypad3, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad4", kHIDUsage_Keypad4, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad5", kHIDUsage_Keypad5, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad6", kHIDUsage_Keypad6, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad7", kHIDUsage_Keypad7, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad8", kHIDUsage_Keypad8, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad9", kHIDUsage_Keypad9, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_Keypad0", kHIDUsage_Keypad0, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadPeriod", kHIDUsage_KeypadPeriod, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardNonUSBackslash", kHIDUsage_KeyboardNonUSBackslash, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardApplication", kHIDUsage_KeyboardApplication, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPower", kHIDUsage_KeyboardPower, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadEqualSign", kHIDUsage_KeypadEqualSign, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF13", kHIDUsage_KeyboardF13, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF14", kHIDUsage_KeyboardF14, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF15", kHIDUsage_KeyboardF15, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF16", kHIDUsage_KeyboardF16, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF17", kHIDUsage_KeyboardF17, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF18", kHIDUsage_KeyboardF18, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF19", kHIDUsage_KeyboardF19, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF20", kHIDUsage_KeyboardF20, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF21", kHIDUsage_KeyboardF21, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF22", kHIDUsage_KeyboardF22, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF23", kHIDUsage_KeyboardF23, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardF24", kHIDUsage_KeyboardF24, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardExecute", kHIDUsage_KeyboardExecute, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardHelp", kHIDUsage_KeyboardHelp, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardMenu", kHIDUsage_KeyboardMenu, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardSelect", kHIDUsage_KeyboardSelect, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardStop", kHIDUsage_KeyboardStop, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardAgain", kHIDUsage_KeyboardAgain, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardUndo", kHIDUsage_KeyboardUndo, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardCut", kHIDUsage_KeyboardCut, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardCopy", kHIDUsage_KeyboardCopy, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPaste", kHIDUsage_KeyboardPaste, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardFind", kHIDUsage_KeyboardFind, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardMute", kHIDUsage_KeyboardMute, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardVolumeUp", kHIDUsage_KeyboardVolumeUp, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardVolumeDown", kHIDUsage_KeyboardVolumeDown, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLockingCapsLock", kHIDUsage_KeyboardLockingCapsLock, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLockingNumLock", kHIDUsage_KeyboardLockingNumLock, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLockingScrollLock", kHIDUsage_KeyboardLockingScrollLock, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadComma", kHIDUsage_KeypadComma, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeypadEqualSignAS400", kHIDUsage_KeypadEqualSignAS400, 0, FORCE_APPLICATION_TO_IGNORE_THIS_KEY );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational1", kHIDUsage_KeyboardInternational1, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational2", kHIDUsage_KeyboardInternational2, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational3", kHIDUsage_KeyboardInternational3, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational4", kHIDUsage_KeyboardInternational4, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational5", kHIDUsage_KeyboardInternational5, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational6", kHIDUsage_KeyboardInternational6, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational7", kHIDUsage_KeyboardInternational7, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational8", kHIDUsage_KeyboardInternational8, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardInternational9", kHIDUsage_KeyboardInternational9, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG1", kHIDUsage_KeyboardLANG1, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG2", kHIDUsage_KeyboardLANG2, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG3", kHIDUsage_KeyboardLANG3, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG4", kHIDUsage_KeyboardLANG4, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG5", kHIDUsage_KeyboardLANG5, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG6", kHIDUsage_KeyboardLANG6, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG7", kHIDUsage_KeyboardLANG7, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG8", kHIDUsage_KeyboardLANG8, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLANG9", kHIDUsage_KeyboardLANG9, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardAlternateErase", kHIDUsage_KeyboardAlternateErase, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardSysReqOrAttention", kHIDUsage_KeyboardSysReqOrAttention, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardCancel", kHIDUsage_KeyboardCancel, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardClear", kHIDUsage_KeyboardClear, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardPrior", kHIDUsage_KeyboardPrior, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardReturn", kHIDUsage_KeyboardReturn, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardSeparator", kHIDUsage_KeyboardSeparator, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardOut", kHIDUsage_KeyboardOut, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardOper", kHIDUsage_KeyboardOper, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardClearOrAgain", kHIDUsage_KeyboardClearOrAgain, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardCrSelOrProps", kHIDUsage_KeyboardCrSelOrProps, 0 );
    PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardExSel", kHIDUsage_KeyboardExSel, 0 );


    // 0xA5-0xDF Reserved
    /*
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLeftControl", kHIDUsage_KeyboardLeftControl, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLeftShift", kHIDUsage_KeyboardLeftShift, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLeftAlt", kHIDUsage_KeyboardLeftAlt, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardLeftGUI", kHIDUsage_KeyboardLeftGUI, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardRightControl", kHIDUsage_KeyboardRightControl, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardRightShift", kHIDUsage_KeyboardRightShift, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardRightAlt", kHIDUsage_KeyboardRightAlt, 0 );
      PerKeyData::PushOneItem( m_keys, "kHIDUsage_KeyboardRightGUI", kHIDUsage_KeyboardRightGUI, 0 );
    */
    // 0xE8-0xFFFF Reserved

    return true; // there isn't a way to "fail", so it's always true. but the structure of Initialize needs us to return bool
}


