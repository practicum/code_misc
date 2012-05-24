
#include <vector>
#include <string>
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>

#include <CoreFoundation/CFString.h>


namespace GitHubSample
{

    /**
       Providing two synchronous ways of reading keyboard state and receiving
       keyboard input.  One way is to poll the keyboard device for the current
       state of each key we care about.  The other way is to synchronously
       retrieve events from a queue.  We can check the queue for events whenever
       we like, and we can configure the maximum queue size in the creation
       function ('create') of the IOHIDQueueInterface. We can decide if it is
       permissible for the event-retrieval call to be blocking or to return
       immediately by using the AbsoluteTime argument to 'getNextEvent'.
     */
    class HelperForKeyboardReaderIOKit
    {
    public:

        explicit HelperForKeyboardReaderIOKit
        (
         bool enableQueue,
         boost::function< void ( const std::string msg ) > errorLoggerFunctor = 0
        );

        /// Will return a NEGATIVE value in case of error.
        int CountOfCurrentlyDepressedKeys() const;

        /// Warning: this seems to receive keypresses that happen even when OUR
        /// APPLICATION is NOT the foreground application
        void ReadFromQueue_Experimental();

    private:

        /// opaque struct. not meant to be used outside this class.
        struct PerKeyData;

        struct PrivateImpl;
        boost::shared_ptr< PrivateImpl > m_pimpl;

        std::vector< boost::shared_ptr<PerKeyData> > m_keys;
        boost::function< void ( const std::string msg ) > m_errorLoggerFunctor;
        std::vector< std::string > m_deviceInformationProperties;
        const bool m_queueEnabled;

        void Initialize();
        void LogInitializationError( const std::string& errorDesc ) const;
        void DebugCheckErrorKeys() const;

        bool FindKeyboard();
        bool CreatePluginInterface();
        void GetKeyboardProperties();
        void StoreOneProperty( CFStringRef propertyKey );
        bool CreateDeviceInterface();
        bool CreateQueue();
        bool PopulateVectorOfKeyInfo();
        bool FindKeypressCookies();
        bool AddElementsToQueue();


        /// declared private so as to make this class non-copyable
        HelperForKeyboardReaderIOKit(const HelperForKeyboardReaderIOKit&);
        /// declared private so as to make this class non-copyable
        HelperForKeyboardReaderIOKit& operator=(const HelperForKeyboardReaderIOKit&);
    };



} // end namespace GitHubSample

