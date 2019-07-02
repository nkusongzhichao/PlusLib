/* -LICENSE-START-
 ** Copyright (c) 2012 Blackmagic Design
 **
 ** Permission is hereby granted, free of charge, to any person or organization
 ** obtaining a copy of the software and accompanying documentation covered by
 ** this license (the "Software") to use, reproduce, display, distribute,
 ** execute, and transmit the Software, and to prepare derivative works of the
 ** Software, and to permit third-parties to whom the Software is furnished to
 ** do so, all subject to the following:
 **
 ** The copyright notices in the Software and this entire statement, including
 ** the above license grant, this restriction and the following disclaimer,
 ** must be included in all copies of the Software, in whole or in part, and
 ** all derivative works of the Software, unless such copies or derivative
 ** works are solely in the form of machine-executable object code generated by
 ** a source language processor.
 **
 ** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 ** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 ** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 ** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 ** DEALINGS IN THE SOFTWARE.
 ** -LICENSE-END-
 */

#include "VideoFrameTransfer.h"


#define DVP_CHECK(cmd) {						\
    DVPStatus hr = (cmd);						\
    if (DVP_STATUS_OK != hr) {                  \
		OutputDebugStringA( #cmd " failed\n" );   \
		ExitProcess(hr);						\
    }                                           \
}


// Initialise static members
bool								VideoFrameTransfer::mInitialized = false;
bool								VideoFrameTransfer::mUseDvp = false;
unsigned							VideoFrameTransfer::mWidth = 0;
unsigned							VideoFrameTransfer::mHeight = 0;
GLuint								VideoFrameTransfer::mCaptureTexture = 0;

// NVIDIA specific static members
DVPBufferHandle						VideoFrameTransfer::mDvpCaptureTextureHandle = 0;
DVPBufferHandle						VideoFrameTransfer::mDvpPlaybackTextureHandle = 0;
uint32_t							VideoFrameTransfer::mBufferAddrAlignment = 0;
uint32_t							VideoFrameTransfer::mBufferGpuStrideAlignment = 0;
uint32_t							VideoFrameTransfer::mSemaphoreAddrAlignment = 0;
uint32_t							VideoFrameTransfer::mSemaphoreAllocSize = 0;
uint32_t							VideoFrameTransfer::mSemaphorePayloadOffset = 0;
uint32_t							VideoFrameTransfer::mSemaphorePayloadSize = 0;


bool VideoFrameTransfer::isNvidiaDvpAvailable()
{
	// Look for supported graphics boards
	const GLubyte* renderer = glGetString(GL_RENDERER);
	bool hasDvp = (strstr((char*)renderer, "Quadro") != NULL);
	return hasDvp;
}

bool VideoFrameTransfer::isAMDPinnedMemoryAvailable()
{
	// GL_AMD_pinned_memory presence indicates GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD buffer target is supported
	const GLubyte* strExt = glGetString(GL_EXTENSIONS);
	bool hasAMDPinned = (strstr((char*)strExt, "GL_AMD_pinned_memory") != NULL);
	return hasAMDPinned;
}

bool VideoFrameTransfer::checkFastMemoryTransferAvailable()
{
	return (isNvidiaDvpAvailable() || isAMDPinnedMemoryAvailable());
}

bool VideoFrameTransfer::initialize(unsigned width, unsigned height, GLuint captureTexture, GLuint playbackTexture)
{
	if (mInitialized)
		return false;

	bool hasDvp = isNvidiaDvpAvailable();
	bool hasAMDPinned = isAMDPinnedMemoryAvailable();

	if (!hasDvp && !hasAMDPinned)
		return false;

	mUseDvp = hasDvp;
	mWidth = width;
	mHeight = height;
	mCaptureTexture = captureTexture;

	if (! initializeMemoryLocking(mWidth * mHeight * 4))		// BGRA uses 4 bytes per pixel
		return false;

	if (mUseDvp)
	{
		// DVP initialisation
		DVP_CHECK(dvpInitGLContext(DVP_DEVICE_FLAGS_SHARE_APP_CONTEXT));
		DVP_CHECK(dvpGetRequiredConstantsGLCtx(	&mBufferAddrAlignment, &mBufferGpuStrideAlignment,
												&mSemaphoreAddrAlignment, &mSemaphoreAllocSize,
												&mSemaphorePayloadOffset, &mSemaphorePayloadSize));
	
		// Register textures with DVP
		DVP_CHECK(dvpCreateGPUTextureGL(captureTexture, &mDvpCaptureTextureHandle));
		DVP_CHECK(dvpCreateGPUTextureGL(playbackTexture, &mDvpPlaybackTextureHandle));
	}

	mInitialized = true;

	return true;
}

bool VideoFrameTransfer::initializeMemoryLocking(unsigned memSize)
{
	// Increase the process working set size to allow pinning of memory.
    static SIZE_T	dwMin = 0, dwMax = 0;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, GetCurrentProcessId());
    if (!hProcess)
		return false;

    // Retrieve the working set size of the process.
    if (!dwMin && !GetProcessWorkingSetSize(hProcess, &dwMin, &dwMax))
		return false;

	// Allow for 80 frames to be locked
	BOOL res = SetProcessWorkingSetSize(hProcess, memSize * 80 + dwMin, memSize * 80 + (dwMax-dwMin));
    if (!res)
		return false;

    CloseHandle(hProcess);
	return true;
}

// SyncInfo sets up a semaphore which is shared between the GPU and CPU and used to
// synchronise access to DVP buffers.
struct SyncInfo
{
	SyncInfo(uint32_t semaphoreAllocSize, uint32_t semaphoreAddrAlignment);
	~SyncInfo();

	volatile uint32_t*	mSem; 
	volatile uint32_t	mReleaseValue; 
	volatile uint32_t	mAcquireValue;
	DVPSyncObjectHandle	mDvpSync;
};

SyncInfo::SyncInfo(uint32_t semaphoreAllocSize, uint32_t semaphoreAddrAlignment)
{
	mSem = (uint32_t*)_aligned_malloc(semaphoreAllocSize, semaphoreAddrAlignment);
    
	// Initialise
	mSem[0] = 0;
	mReleaseValue = 0;
	mAcquireValue = 0;

	// Setup DVP sync object and import it
	DVPSyncObjectDesc syncObjectDesc;
	syncObjectDesc.externalClientWaitFunc = NULL;
	syncObjectDesc.sem = (uint32_t*)mSem;

	DVP_CHECK(dvpImportSyncObject(&syncObjectDesc, &mDvpSync));
}

SyncInfo::~SyncInfo()
{
	DVP_CHECK(dvpFreeSyncObject(mDvpSync));
	_aligned_free((void*)mSem);
}

VideoFrameTransfer::VideoFrameTransfer(unsigned long memSize, void* address, Direction direction) :
	mBuffer(address),
	mMemSize(memSize),
	mDirection(direction),
	mExtSync(NULL),
	mGpuSync(NULL),
	mDvpSysMemHandle(0),
	mBufferHandle(0)
{
	if (mUseDvp)
	{
		// Pin the memory
		if (! VirtualLock(mBuffer, mMemSize))
			throw std::runtime_error("Error pinning memory with VirtualLock");

		// Create necessary sysmem and gpu sync objects
		mExtSync = new SyncInfo(mSemaphoreAllocSize, mSemaphoreAddrAlignment);
		mGpuSync = new SyncInfo(mSemaphoreAllocSize, mSemaphoreAddrAlignment);

		// Register system memory buffers with DVP
		DVPSysmemBufferDesc sysMemBuffersDesc;
		sysMemBuffersDesc.width		= mWidth;
		sysMemBuffersDesc.height	= mHeight;
		sysMemBuffersDesc.stride	= mWidth * 4;
		sysMemBuffersDesc.format	= DVP_BGRA;
		sysMemBuffersDesc.type		= DVP_UNSIGNED_BYTE;
		sysMemBuffersDesc.size		= mMemSize;
		sysMemBuffersDesc.bufAddr	= mBuffer;

		if (mDirection == CPUtoGPU)
		{
			// A UYVY 4:2:2 frame is transferred to the GPU, rather than RGB 4:4:4, so width is halved
			sysMemBuffersDesc.width /= 2;
			sysMemBuffersDesc.stride /= 2;
		}

		DVP_CHECK(dvpCreateBuffer(&sysMemBuffersDesc, &mDvpSysMemHandle));
		DVP_CHECK(dvpBindToGLCtx(mDvpSysMemHandle));
	}
	else
	{
		// Create an OpenGL buffer handle to use for pinned memory
		GLuint bufferHandle;
		glGenBuffers(1, &bufferHandle);

		// Pin memory by binding buffer to special AMD target.
		glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, bufferHandle);

		// glBufferData() sets up the address so any OpenGL operation on this buffer will use system memory directly
		// (assumes address is aligned to 4k boundary).
		glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, mMemSize, address, GL_STREAM_DRAW);
		GLenum result = glGetError();
		if (result != GL_NO_ERROR)
		{
			throw std::runtime_error("Error pinning memory with glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, ...)");
		}
		glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 0);		// Unbind buffer to target

		mBufferHandle = bufferHandle;
	}
}

VideoFrameTransfer::~VideoFrameTransfer()
{
	if (mUseDvp)
	{
		DVP_CHECK(dvpUnbindFromGLCtx(mDvpSysMemHandle));
		DVP_CHECK(dvpDestroyBuffer(mDvpSysMemHandle));

		delete mExtSync;
		delete mGpuSync;

		VirtualUnlock(mBuffer, mMemSize);
	}
	else
	{
		// The buffer is un-pinned by the GPU when the buffer is deleted
		glDeleteBuffers(1, &mBufferHandle);
	}
}

bool VideoFrameTransfer::performFrameTransfer()
{
	if (mUseDvp)
	{
		// NVIDIA DVP transfers
		DVPStatus status;

		mGpuSync->mReleaseValue++;

		dvpBegin();
		if (mDirection == CPUtoGPU)
		{
			// Copy from system memory to GPU texture
			dvpMapBufferWaitDVP(mDvpCaptureTextureHandle);
			status = dvpMemcpyLined(	mDvpSysMemHandle, mExtSync->mDvpSync, mExtSync->mAcquireValue, DVP_TIMEOUT_IGNORED,
										mDvpCaptureTextureHandle, mGpuSync->mDvpSync, mGpuSync->mReleaseValue, 0, mHeight);
			dvpMapBufferEndDVP(mDvpCaptureTextureHandle);
		}
		else
		{
			// Copy from GPU texture to system memory
			dvpMapBufferWaitDVP(mDvpPlaybackTextureHandle);
			status = dvpMemcpyLined(	mDvpPlaybackTextureHandle, mExtSync->mDvpSync, mExtSync->mReleaseValue, DVP_TIMEOUT_IGNORED,
										mDvpSysMemHandle, mGpuSync->mDvpSync, mGpuSync->mReleaseValue, 0, mHeight);
			dvpMapBufferEndDVP(mDvpPlaybackTextureHandle);
		}
		dvpEnd();

		return (status == DVP_STATUS_OK);
	}
	else
	{
		// AMD pinned memory transfers
		if (mDirection == CPUtoGPU)
		{
			glEnable(GL_TEXTURE_2D);

			// Use a pinned buffer for the GL_PIXEL_UNPACK_BUFFER target
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, mBufferHandle);
			glBindTexture(GL_TEXTURE_2D, mCaptureTexture);

			// NULL for last arg indicates use current GL_PIXEL_UNPACK_BUFFER target as texture data
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mWidth/2, mHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

			// Ensure pinned texture has been transferred to GPU before we draw with it
			GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 40 * 1000 * 1000);	// timeout in nanosec
			glDeleteSync(fence);

			glBindTexture(GL_TEXTURE_2D, 0);
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			glDisable(GL_TEXTURE_2D);
		}
		else
		{
			// Use a PIXEL PACK BUFFER to read back pixels
			glBindBuffer(GL_PIXEL_PACK_BUFFER, mBufferHandle);
			glReadPixels(0, 0, mWidth, mHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

			// Ensure GPU has processed all commands in the pipeline up to this point, before memory is read by the CPU
			GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 40 * 1000 * 1000);	// timeout in nanosec
			glDeleteSync(fence);
		}

		return (glGetError() == GL_NO_ERROR);
	}
}

void VideoFrameTransfer::waitForTransferComplete()
{
	if (!mUseDvp)
		return;

	// Block until buffer has completely transferred from GPU to CPU buffer
	if (mDirection == GPUtoCPU)
	{
		dvpBegin();
		dvpSyncObjClientWaitComplete(mGpuSync->mDvpSync, DVP_TIMEOUT_IGNORED);
		dvpEnd();
	}
}

void VideoFrameTransfer::beginTextureInUse(Direction direction)
{
	if (!mUseDvp)
		return;

	if (direction == CPUtoGPU)
		dvpMapBufferWaitAPI(mDvpCaptureTextureHandle);
	else
		dvpMapBufferWaitAPI(mDvpPlaybackTextureHandle);
}

void VideoFrameTransfer::endTextureInUse(Direction direction)
{
	if (!mUseDvp)
		return;

	if (direction == CPUtoGPU)
		dvpMapBufferEndAPI(mDvpCaptureTextureHandle);
	else
		dvpMapBufferEndAPI(mDvpPlaybackTextureHandle);
}
