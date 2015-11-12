//
//  DawStateSingleton.cpp
//  WebCore
//
//  Created by Nikolay Tsenkov on 10/26/15.
//
//

// Do I need this? Perhaps for ENABLE(WEB_AUDIO)?
#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "DawStateSingleton.h"

#include <wtf/NeverDestroyed.h>

namespace WebCore {

    DawStateSingleton& DawStateSingleton::getInstance()
    {
        static NeverDestroyed<DawStateSingleton> instance;
        return instance;
    }

    float DawStateSingleton::samplingRate()
    {
        return m_samplingRate;
    }
    void DawStateSingleton::setSamplingRate(float samplingRate)
    {
        m_samplingRate = samplingRate;
    }

    IAudioRenderOutput* DawStateSingleton::audioOutput()
    {
        return m_audioOutput;
    }
    void DawStateSingleton::setAudioOutput( IAudioRenderOutput* audioOutput )
    {
        m_audioOutput = audioOutput;
    }

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
