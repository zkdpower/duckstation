#include "cdrom_async_reader.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/timer.h"
Log_SetChannel(CDROMAsyncReader);

CDROMAsyncReader::CDROMAsyncReader() = default;

CDROMAsyncReader::~CDROMAsyncReader()
{
  StopThread();
}

void CDROMAsyncReader::StartThread(u32 readahead_count)
{
  if (IsUsingThread())
    return;

  m_buffers.clear();
  AllocateBuffers(readahead_count);

  m_shutdown_flag.store(false);
  m_read_thread = std::thread(&CDROMAsyncReader::WorkerThreadEntryPoint, this);
}

void CDROMAsyncReader::StopThread()
{
  if (!IsUsingThread())
    return;

  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_shutdown_flag.store(true);
    m_do_read_cv.notify_one();
  }

  m_read_thread.join();
  m_buffers.clear();
}

void CDROMAsyncReader::SetMedia(std::unique_ptr<CDImage> media)
{
  CancelReadahead();
  m_media = std::move(media);
}

std::unique_ptr<CDImage> CDROMAsyncReader::RemoveMedia()
{
  CancelReadahead();
  return std::move(m_media);
}

void CDROMAsyncReader::QueueReadSector(CDImage::LBA lba)
{
  if (!IsUsingThread())
  {
    m_seek_error.store(false);
    m_next_position_set.store(true);
    m_next_position = lba;
    return;
  }

  std::unique_lock<std::mutex> lock(m_mutex);
  const u32 buffer_count = m_buffer_count.load();
  if (buffer_count > 0)
  {
    // don't re-read the same sector if it was the last one we read
    // the CDC code does this when seeking->reading
    const u32 buffer_front = m_buffer_front.load();
    if (m_buffers[buffer_front].lba == lba)
    {
      Log_DebugPrintf("Skipping re-reading same sector %u", lba);
      return;
    }

    // did we readahead to the correct sector?
    const u32 next_buffer = (buffer_front + 1) % static_cast<u32>(m_buffers.size());
    if (m_buffer_count > 1 && m_buffers[next_buffer].lba == lba)
    {
      // great, don't need a seek, but still kick the thread to start reading ahead again
      Log_DebugPrintf("Readahead buffer hit for sector %u", lba);
      m_buffer_front.store(next_buffer);
      m_buffer_count.fetch_sub(1);
      m_can_readahead.store(true);
      m_do_read_cv.notify_one();
      return;
    }
  }

  // we need to toss away our readahead and start fresh
  Log_DebugPrintf("Readahead buffer miss, queueing seek to %u", lba);
  m_next_position_set.store(true);
  m_next_position = lba;
  m_do_read_cv.notify_one();
}

bool CDROMAsyncReader::ReadSectorUncached(CDImage::LBA lba, CDImage::SubChannelQ* subq, SectorBuffer* data)
{
  CancelReadahead();

  if (m_media->GetPositionOnDisc() != lba && !m_media->Seek(lba))
  {
    Log_WarningPrintf("Seek to LBA %u failed", lba);
    return false;
  }

  if (!m_media->ReadRawSector(data, subq))
  {
    Log_WarningPrintf("Read of LBA %u failed", lba);
    return false;
  }

  return true;
}

bool CDROMAsyncReader::WaitForReadToComplete()
{
  //   if (!IsUsingThread())
  //     return m_sector_read_result.load();

  // TODO: Verify this is safe on ARM. It should be.
  if (!m_next_position_set.load() && m_buffer_count.load() > 0)
  {
    Log_TracePrintf("Returning sector %u", m_buffers[m_buffer_front.load()].lba);
    return m_buffers[m_buffer_front.load()].result;
  }

  Common::Timer wait_timer;
  std::unique_lock<std::mutex> lock(m_mutex);
  Log_DebugPrintf("Sector read pending, waiting");

  m_notify_read_complete_cv.wait(lock, [this]() { return m_buffer_count.load() > 0 && !m_next_position_set.load(); });

  const u32 front = m_buffer_front.load();
  const double wait_time = wait_timer.GetTimeMilliseconds();
  if (wait_time > 1.0f)
    Log_WarningPrintf("Had to wait %.2f msec for LBA %u", wait_time, m_buffers[front].lba);

  Log_TracePrintf("Returning sector %u after waiting", m_buffers[front].lba);
  return m_buffers[front].result;
}

void CDROMAsyncReader::AllocateBuffers(u32 count)
{
  m_buffers.resize(count);
  m_buffer_front.store(0);
  m_buffer_back.store(0);
  m_buffer_count.store(0);
}

void CDROMAsyncReader::EmptyBuffers()
{
  m_buffer_front.store(0);
  m_buffer_back.store(0);
  m_buffer_count.store(0);
}

bool CDROMAsyncReader::ReadSectorIntoBuffer(std::unique_lock<std::mutex>& lock)
{
  Common::Timer timer;

  const u32 slot = m_buffer_back.load();
  m_buffer_back.store((slot + 1) % static_cast<u32>(m_buffers.size()));

  BufferSlot& buffer = m_buffers[slot];
  buffer.lba = m_media->GetPositionOnDisc();
  m_is_reading.store(true);
  lock.unlock();

  Log_TracePrintf("Reading LBA %u...", buffer.lba);

  buffer.result = m_media->ReadRawSector(buffer.data.data(), &buffer.subq);
  if (buffer.result)
  {
    const double read_time = timer.GetTimeMilliseconds();
    if (read_time > 1.0f)
      Log_DevPrintf("Read LBA %u took %.2f msec", buffer.lba, read_time);
  }
  else
  {
    Log_ErrorPrintf("Read of LBA %u failed", buffer.lba);
  }

  lock.lock();
  m_is_reading.store(false);
  m_buffer_count.fetch_add(1);
  m_notify_read_complete_cv.notify_all();
  return true;
}

void CDROMAsyncReader::CancelReadahead()
{
  Log_DevPrintf("Cancelling readahead");

  std::unique_lock lock(m_mutex);

  // wait until the read thread is idle
  m_notify_read_complete_cv.wait(lock, [this]() { return !m_is_reading.load(); });

  // prevent it from doing any more when it re-acquires the lock
  m_can_readahead.store(false);
  EmptyBuffers();
}

void CDROMAsyncReader::WorkerThreadEntryPoint()
{
  std::unique_lock lock(m_mutex);

  for (;;)
  {
    m_do_read_cv.wait(
      lock, [this]() { return (m_shutdown_flag.load() || m_next_position_set.load() || m_can_readahead.load()); });
    if (m_shutdown_flag.load())
      break;

    for (;;)
    {
      if (m_next_position_set.load())
      {
        // discard buffers, we're seeking to a new location
        const CDImage::LBA seek_location = m_next_position.load();
        EmptyBuffers();
        m_next_position_set.store(false);
        m_seek_error.store(false);
        lock.unlock();

        // seek without lock held in case it takes time
        Log_DebugPrintf("Seeking to LBA %u...", seek_location);
        const bool seek_result = (m_media->GetPositionOnDisc() == seek_location || m_media->Seek(seek_location));

        lock.lock();

        // did another request come in? abort if so
        if (m_next_position_set.load())
          continue;

        // did we fail the seek?
        if (!seek_result)
        {
          // add the error result, and don't try to read ahead
          Log_WarningPrintf("Seek to LBA %u failed", seek_location);
          m_seek_error.store(true);
          break;
        }

        // go go read ahead!
        m_can_readahead.store(true);
      }

      if (!m_can_readahead.load())
        break;

      // readahead time! read as many sectors as we have space for
      const u32 read_count = static_cast<u32>(m_buffers.size()) - m_buffer_count.load();
      Log_DebugPrintf("Reading ahead %u sectors...", read_count);
      for (u32 i = 0; i < read_count; i++)
      {
        if (m_next_position_set.load())
        {
          // a seek request came in while we're reading, so bail out
          break;
        }

        // stop reading if we hit the end or get an error
        if (!ReadSectorIntoBuffer(lock))
          break;
      }

      // readahead buffer is full or errored at this point
      m_can_readahead.store(false);
      break;
    }
  }
}
