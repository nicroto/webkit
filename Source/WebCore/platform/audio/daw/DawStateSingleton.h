//
//  DawStateSingleton.hpp
//  WebCore
//
//  Created by Nikolay Tsenkov on 10/26/15.
//
//

#ifndef DawStateSingleton_h
#define DawStateSingleton_h

#include "IAudioRenderOutput.h"

#include <wtf/Forward.h>

namespace WebCore {

    class DawStateSingleton {

        friend class WTF::NeverDestroyed<DawStateSingleton>;

        public:
            WEBCORE_EXPORT static DawStateSingleton& getInstance();

            WEBCORE_EXPORT float samplingRate();
            WEBCORE_EXPORT void setSamplingRate( float samplingRate );

            WEBCORE_EXPORT IAudioRenderOutput* audioOutput();
            void setAudioOutput( IAudioRenderOutput* audioOutput );

        private:
            DawStateSingleton(){};
            DawStateSingleton(DawStateSingleton const&);    // Don't Implement.
            void operator=(DawStateSingleton const&);       // Don't implement

            // fields
            float m_samplingRate = 44100;
            IAudioRenderOutput* m_audioOutput;

    };

} // namespace WebCore

#endif /* DawStateSingleton_h */
