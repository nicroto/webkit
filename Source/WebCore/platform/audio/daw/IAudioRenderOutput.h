//
//  IAudioRenderOutput.h
//  WebCore
//
//  Created by Nikolay Tsenkov on 10/28/15.
//
//

#ifndef IAudioRenderOutput_h
#define IAudioRenderOutput_h

#include <AudioUnit/AudioUnit.h>

namespace WebCore {

class IAudioRenderOutput
{
    public:
        virtual ~IAudioRenderOutput() { }

        virtual OSStatus render(UInt32, AudioBufferList*) = 0;
    
};

} // namespace WebCore

#endif /* IAudioRenderOutput_h */
