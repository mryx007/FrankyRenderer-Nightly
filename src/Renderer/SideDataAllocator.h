#pragma once
#include "Renderer/MediaSampleSideData.h"
#include <algorithm>

// The actual MPC/LAV side-data sample and MPC allocation loop are reused.
// No MPC pixel processing or decoder implementation is involved.
class SideDataSample final : public CMediaSampleSideData
{
  public:
    SideDataSample(LPCTSTR name, CBaseAllocator* allocator, HRESULT* hr, LPBYTE bytes, LONG length)
        : CMediaSampleSideData(name, allocator, hr, bytes, length)
    {
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return CMediaSample::AddRef();
    }
};

class SideDataAllocator final : public CMemAllocator
{
    // A media type the upstream filter accepted in QueryAccept, handed to it
    // with the next buffer (DirectShow dynamic format change from downstream).
    CMediaType pending;
    bool hasPending = false;
    // The CPU Queue holds decoded pictures: the decoder needs that many
    // buffers more than it asks for.
    long minimumBuffers = 0;

  protected:
    HRESULT Alloc() override;

  public:
    explicit SideDataAllocator(HRESULT* hr) : CMemAllocator(NAME("libplacebo side-data allocator"), nullptr, hr) {}

    void SetMinimumBuffers(long count)
    {
        CAutoLock guard(this);
        minimumBuffers = count;
    }

    STDMETHODIMP SetProperties(ALLOCATOR_PROPERTIES* request, ALLOCATOR_PROPERTIES* actual) override
    {
        if (!request)
            return E_POINTER;
        ALLOCATOR_PROPERTIES wanted = *request;
        {
            CAutoLock guard(this);
            wanted.cBuffers = (std::max)(wanted.cBuffers, minimumBuffers);
        }
        return CMemAllocator::SetProperties(&wanted, actual);
    }

    void SetPendingType(const CMediaType& type)
    {
        CAutoLock guard(this);
        pending = type;
        hasPending = true;
    }

    // Makes every buffer hold at least this many bytes, for a filter that
    // reconnects with a larger picture. Fails while buffers are still in use.
    HRESULT Grow(long bytes)
    {
        ALLOCATOR_PROPERTIES have = {}, actual = {};
        HRESULT hr = GetProperties(&have);
        if (FAILED(hr) || have.cbBuffer >= bytes)
            return hr;
        const bool committed = m_bCommitted != FALSE;
        if (committed)
            Decommit();
        have.cbBuffer = bytes;
        hr = SetProperties(&have, &actual);
        if (committed)
        {
            const HRESULT again = Commit();
            if (SUCCEEDED(hr))
                hr = again;
        }
        return hr;
    }

    STDMETHODIMP GetBuffer(IMediaSample** sample, REFERENCE_TIME* start, REFERENCE_TIME* end, DWORD flags) override
    {
        HRESULT hr = CMemAllocator::GetBuffer(sample, start, end, flags);
        if (FAILED(hr) || !sample || !*sample)
            return hr;
        CAutoLock guard(this);
        if (hasPending && SUCCEEDED((*sample)->SetMediaType(&pending)))
            hasPending = false;
        return hr;
    }
};
