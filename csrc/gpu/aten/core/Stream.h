#pragma once

#include <cstdint>
#include <utility>

#include <c10/core/DeviceGuard.h>
#include <c10/core/Stream.h>
#include <utils/Macros.h>

/*
 * A DPCPPStream is an abstraction of an actual sycl queue on the XPU.
 * DPCPPStreams are backed by sycl queue, but they use several pools to minimize
 * the costs associated with creating, retaining, and destroying sycl queues.
 */

using namespace at;

namespace xpu {
namespace dpcpp {

// Please keep synchronized with QueueIndex in runtime/Queue.h
using QueueIndex = uint8_t;

// This is a wrapper around c10::Stream. And use DPCPPStream.id() to unpack a
// QueueIndex to retrieve sycl queue from the pool.
class IPEX_API DPCPPStream {
 public:
  enum Unchecked { UNCHECKED };

  /// Construct a DPCPPStream from a Stream.  This construction is checked, and
  /// will raise an error if the Stream is not, in fact, a DPCPP stream.
  explicit DPCPPStream(Stream stream) : stream_(stream) {
    TORCH_CHECK(stream_.device_type() == DeviceType::XPU);
  }

  /// Construct a DPCPPStream from a Stream with no error checking.
  explicit DPCPPStream(Unchecked, Stream stream) : stream_(stream) {}

  bool operator==(const DPCPPStream& other) const noexcept {
    return unwrap() == other.unwrap();
  }

  bool operator!=(const DPCPPStream& other) const noexcept {
    return unwrap() != other.unwrap();
  }

  /// Implicit conversion to Stream.
  operator Stream() const {
    return unwrap();
  }

  /// Used to avoid baking in device type explicitly to Python-side API.
  DeviceType device_type() const {
    return DeviceType::XPU;
  }

  /// Get the XPU device index that this stream is associated with.
  DeviceIndex device_index() const {
    return stream_.device_index();
  }

  /// Get the full Device that this stream is associated with. The Device is
  /// guaranteed to be a XPU device.
  Device device() const {
    return Device(DeviceType::XPU, device_index());
  }

  /// Return the stream ID corresponding to this particular stream. StreamId is
  /// a int64_t representation generated by QueueIndex and QueueType.
  StreamId id() const {
    return stream_.id();
  }

  void synchronize() const;

  void synchronize_and_throw() const;

  /// Explicit conversion to sycl queue opaque pointer.
  void* queue() const;

  /// Return the sycl queue index in the queue pool.
  QueueIndex queue_index() const;

  /// Explicit conversion to Stream.
  Stream unwrap() const {
    return stream_;
  }

  /// Reversibly pack a DPCPPStream into a StreamData3 representation.
  struct c10::StreamData3 pack3() const {
    return stream_.pack3();
  }

  /// Unpack a DPCPPStream from the 3 fields generated by pack().
  static DPCPPStream unpack3(
      StreamId stream_id,
      DeviceIndex device_index,
      DeviceType device_type) {
    return DPCPPStream(Stream::unpack3(stream_id, device_index, device_type));
  }

 private:
  Stream stream_;
};

/**
 * Get a new stream associated with sycl queue index in sycl queue pool. You can
 * think of this as "creating" a new queue, but no such creation actually
 * happens; Instead, queues are preallocated from the pool and returned in a
 * round-robin fashion.
 *
 * Currently, priority queue property is not supported yet. You can request a
 * stream for a specific device by setting device.
 */
IPEX_API DPCPPStream
getStreamFromPool(const bool isHighPriority = false, DeviceIndex device = -1);

/**
 * Get the current XPU stream, for the passed XPU device, or for the
 * current device if no device index is passed.
 */
IPEX_API DPCPPStream getCurrentDPCPPStream(DeviceIndex device_index = -1);

/**
 * Set the current stream on the device of the passed in stream to be the passed
 * in stream.
 *
 * Avoid using this function; Prefer using 'DPCPPStreamGuard' instead, which
 * will switch both your current device and current stream in the way you
 * expect, and reset it back to its original state afterwards).
 */
IPEX_API void setCurrentDPCPPStream(DPCPPStream stream);

/**
 * Block all queues on the device, and wait their synchronizations. We emulate
 * the semantics via a loop through the queue pool of the specified device and
 * make each command queue synchronization sequentially. Obviously, the latency
 * depends on the most time-consuming queue as each of the queue is executed
 * asynchronously.
 */
IPEX_API void deviceSynchronize(DeviceIndex device_index = -1);

} // namespace dpcpp
} // namespace xpu

using namespace xpu::dpcpp;

namespace std {
template <>
struct hash<xpu::dpcpp::DPCPPStream> {
  size_t operator()(xpu::dpcpp::DPCPPStream s) const noexcept {
    return std::hash<c10::Stream>{}(s.unwrap());
  }
};
} // namespace std
