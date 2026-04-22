#include <xf86drm.h>
#include <sys/eventfd.h>

#include "Timeline.h"
#include "wlserver.hpp"
#include "rendervulkan.hpp"
#include "convar.h"

#include "wlr_begin.hpp"
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include "wlr_end.hpp"

namespace gamescope
{
    static LogScope s_TimelineLog( "timeline" );
    static gamescope::ConVar<uint32_t> cv_timeline_wait_timeout_ms( "timeline_wait_timeout_ms", 15000, "Timeout for timeline waits in milliseconds when callers request an infinite wait. Set to 0 for infinite." );

    static uint32_t SyncobjFdToHandle( int32_t nFd )
    {
        int32_t nRet;
        uint32_t uHandle = 0;
        if ( ( nRet = drmSyncobjFDToHandle( g_device.drmRenderFd(), nFd, &uHandle ) ) < 0 )
            return 0;

        return uHandle;
    }

    /*static*/ std::shared_ptr<CTimeline> CTimeline::Create( const TimelineCreateDesc_t &desc )
    {
        if ( g_device.isNvidiaDevice() )
        {
            uint32_t uHandle = 0;
            if ( drmSyncobjCreate( g_device.drmRenderFd(), 0, &uHandle ) != 0 )
            {
                s_TimelineLog.errorf_errno( "CTimeline::Create (NVIDIA): drmSyncobjCreate failed" );
                return nullptr;
            }

            int32_t nFd = -1;
            if ( drmSyncobjHandleToFD( g_device.drmRenderFd(), uHandle, &nFd ) != 0 )
            {
                s_TimelineLog.errorf_errno( "CTimeline::Create (NVIDIA): drmSyncobjHandleToFD failed" );
                drmSyncobjDestroy( g_device.drmRenderFd(), uHandle );
                return nullptr;
            }

            if ( desc.ulStartingPoint > 0 )
            {
                uint64_t ulPoint = desc.ulStartingPoint;
                if ( drmSyncobjTimelineSignal( g_device.drmRenderFd(), &uHandle, &ulPoint, 1 ) != 0 )
                {
                    s_TimelineLog.errorf_errno( "CTimeline::Create (NVIDIA): drmSyncobjTimelineSignal failed" );
                    close( nFd );
                    drmSyncobjDestroy( g_device.drmRenderFd(), uHandle );
                    return nullptr;
                }
            }

            return std::make_shared<CTimeline>( nFd, uHandle, nullptr );
        }

        std::shared_ptr<VulkanTimelineSemaphore_t> pSemaphore = g_device.CreateTimelineSemaphore( desc.ulStartingPoint, true );
        if ( !pSemaphore )
            return nullptr;

        return std::make_shared<CTimeline>( pSemaphore->GetFd(), std::move( pSemaphore ) );
    }

    CTimeline::CTimeline( int32_t nSyncobjFd, std::shared_ptr<VulkanTimelineSemaphore_t> pSemaphore )
        : CTimeline( nSyncobjFd, SyncobjFdToHandle( nSyncobjFd ), std::move( pSemaphore ) )
    {
    }

    CTimeline::CTimeline( int32_t nSyncobjFd, uint32_t uSyncobjHandle, std::shared_ptr<VulkanTimelineSemaphore_t> pSemaphore )
        : m_nSyncobjFd{ nSyncobjFd }
        , m_uSyncobjHandle{ uSyncobjHandle }
        , m_pVkSemaphore{ std::move( pSemaphore ) }
    {
    }

    CTimeline::~CTimeline()
    {
        m_pVkSemaphore = nullptr;

        if ( m_uSyncobjHandle )
            drmSyncobjDestroy( GetDrmRenderFD(), m_uSyncobjHandle );
        if ( m_nSyncobjFd >= 0 )
            close( m_nSyncobjFd );
    }

    int32_t CTimeline::GetDrmRenderFD()
    {
        return g_device.drmRenderFd();
    }

    std::shared_ptr<VulkanTimelineSemaphore_t> CTimeline::ToVkSemaphore()
    {
        if ( g_device.isNvidiaDevice() )
        {
            s_TimelineLog.errorf( "CTimeline::ToVkSemaphore called on NVIDIA timeline. Refusing OPAQUE_FD timeline import path." );
            return nullptr;
        }

        if ( !m_pVkSemaphore )
            m_pVkSemaphore = g_device.ImportTimelineSemaphore( this );

        return m_pVkSemaphore;
    }

    std::shared_ptr<VulkanTimelineSemaphore_t> CTimeline::ImportPointAsBinary( uint64_t ulPoint )
    {
        return g_device.ImportSyncPointAsBinary( GetDrmRenderFD(), m_uSyncobjHandle, ulPoint );
    }

    std::shared_ptr<VulkanTimelineSemaphore_t> CTimeline::CreateSignalSemaphoreForPoint( uint64_t ulPoint )
    {
        return g_device.CreateExportableBinarySemaphore( GetDrmRenderFD(), m_uSyncobjHandle, ulPoint );
    }

    // CTimelinePoint

    template <TimelinePointType Type>
    CTimelinePoint<Type>::CTimelinePoint( std::shared_ptr<CTimeline> pTimeline, uint64_t ulPoint  )
        : m_pTimeline{ std::move( pTimeline ) }
        , m_ulPoint{ ulPoint }
    {
    }

    template <TimelinePointType Type>
    CTimelinePoint<Type>::~CTimelinePoint()
    {
        if ( ShouldSignalOnDestruction() )
        {
            uint32_t uHandle = m_pTimeline->GetSyncobjHandle();
            if ( drmSyncobjTimelineSignal( m_pTimeline->GetDrmRenderFD(), &uHandle, &m_ulPoint, 1 ) != 0 )
            {
                s_TimelineLog.errorf_errno( "CTimelinePoint::~CTimelinePoint drmSyncobjTimelineSignal failed (syncobj 0x%x point %llu)",
                    uHandle, (unsigned long long)m_ulPoint );
            }
        }
    }

    template <TimelinePointType Type>
    bool CTimelinePoint<Type>::Wait( int64_t lTimeout )
    {
        uint32_t uHandle = m_pTimeline->GetSyncobjHandle();
        if ( lTimeout == std::numeric_limits<int64_t>::max() && cv_timeline_wait_timeout_ms > 0u )
        {
            lTimeout = int64_t( uint64_t( cv_timeline_wait_timeout_ms ) * 1000000ull );
        }

        int nRet = drmSyncobjTimelineWait(
            m_pTimeline->GetDrmRenderFD(),
            &uHandle,
            &m_ulPoint,
            1,
            lTimeout,
            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
            nullptr );

        if ( nRet != 0 )
        {
            s_TimelineLog.errorf( "drmSyncobjTimelineWait failed/timed out (ret=%d syncobj=0x%x point=%llu timeout_ms=%u)",
                nRet, uHandle, (unsigned long long)m_ulPoint,
                lTimeout == std::numeric_limits<int64_t>::max() ? 0u : (uint32_t)( lTimeout / 1000000ull ) );
        }

        return nRet == 0;
    }

    //
    // Fence flags tl;dr
    // 0                                      -> Wait for signal on a materialized fence, -ENOENT if not materialized
    // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE  -> Wait only for materialization
    // DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT -> Wait for materialization + signal
    //
    template <TimelinePointType Type>
    std::pair<int32_t, bool> CTimelinePoint<Type>::CreateEventFd()
    {
        assert( Type == TimelinePointType::Acquire );

        uint32_t uHandle = m_pTimeline->GetSyncobjHandle();
        uint64_t ulSignalledPoint = 0;
        int nRet = drmSyncobjQuery( m_pTimeline->GetDrmRenderFD(), &uHandle, &ulSignalledPoint, 1u );
        if ( nRet != 0 )
        {
            s_TimelineLog.errorf_errno( "drmSyncobjQuery failed (%d)", nRet );
            return k_InvalidEvent;
        }

        if ( ulSignalledPoint >= m_ulPoint )
        {
            return k_AlreadySignalledEvent;
        }
        else
        {
            const int32_t nExplicitSyncEventFd = eventfd( 0, EFD_CLOEXEC );
            if ( nExplicitSyncEventFd < 0 )
            {
                s_TimelineLog.errorf_errno( "Failed to create eventfd (%d)", nExplicitSyncEventFd );
                return k_InvalidEvent;
            }

            drm_syncobj_eventfd syncobjEventFd =
            {
                .handle = m_pTimeline->GetSyncobjHandle(),
                // Only valid flags are: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE
                // -> Wait for fence materialization rather than signal.
                .flags  = 0u,
                .point  = m_ulPoint,
                .fd     = nExplicitSyncEventFd,
            };

            if ( drmIoctl( m_pTimeline->GetDrmRenderFD(), DRM_IOCTL_SYNCOBJ_EVENTFD, &syncobjEventFd ) != 0 )
            {
                s_TimelineLog.errorf_errno( "DRM_IOCTL_SYNCOBJ_EVENTFD failed" );
                close( nExplicitSyncEventFd );
                return k_InvalidEvent;
            }

            return { nExplicitSyncEventFd, false };
        }
    }

    template class CTimelinePoint<TimelinePointType::Acquire>;
    template class CTimelinePoint<TimelinePointType::Release>;

}
