#ifdef UDIPE_BUILD_BENCHMARKS

    #pragma once

    //! \file
    //! \brief Pool of distributions for ergonomic allocation reuse
    //!
    //! By design, a \ref distribution_t is meant to be a relatively short-lived
    //! object, which serves a simple statistical analysis purpose then is
    //! discarded. However, this does not mean that the underlying memory
    //! allocation has to be short-lived, as distribution_reset() lets you
    //! recycle said allocation into a \ref distribution_builder_t for the
    //! purpose of building another \ref distribution_t later on. But one
    //! drawback of distribution_reset() is that it can make code rather
    //! confusing.
    //!
    //! Acknowledging this, this code module provides the \ref
    //! distribution_pool_t object, which lets you more easily recycle
    //! distributions by abstracting away the reuse cycle:
    //!
    //! - When you are done with a certain \ref distribution_t, you hand it over
    //!   to the pool with distribution_pool_recycle(), and it will be recycled
    //!   into an empty \ref distribution_builder_t available for later reuse.
    //! - When you need an empty \ref distribution_builder_t, you ask the pool
    //!   for one with distribution_pool_request(), and it will either hand
    //!   over one of the previously recycled distribution builders for you, or
    //!   allocate a new one if no builder is currently available.

    #include <udipe/pointer.h>

    #include "distribution.h"


    /// Distribution pool for ergonomic distribution recycling
    ///
    /// C++ programmers will recognize the inner data structure as the moral
    /// equivalent of an `std::vector<distribution_builder_t>`.
    typedef struct distribution_pool_s {
        distribution_builder_t* builders;  ///< Recycled distribution builders
        size_t capacity;  ///< Size of the array targeted by `builders`
        size_t length;  ///< Number of usable builders in `builders`
    } distribution_pool_t;

    /// Allocate a distribution pool
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \returns a distribution pool that must later be liberated using
    ///          distribution_pool_finalize().
    UDIPE_NON_NULL_ARGS
    distribution_pool_t distribution_pool_initialize();

    /// Request a distribution builder from a distribution pool
    ///
    /// If a distribution builder is available in the pool, it will be returned,
    /// otherwise a new distribution builder will be allocated and returned.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param pool must be a \ref distribution_pool_t that was previously built
    ///             using distribution_pool_initialize() and hasn't been
    ///             destroyed since.
    ///
    /// \returns an empty \ref distribution_builder_t that can be used as if it
    ///          were freshly allocated by distribution_initialize().
    UDIPE_NON_NULL_ARGS
    distribution_builder_t distribution_pool_request(distribution_pool_t* pool);


    /// Submit a distribution to a distribution pool for recycling
    ///
    /// Much like distribution_reset(), this has the effect of making the
    /// original \ref distribution_t unusable, and should therefore only be
    /// called at the point where you won't need `dist` anymore.
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param pool must be a \ref distribution_pool_t that was previously built
    ///             using distribution_pool_initialize() and hasn't been
    ///             destroyed since.
    /// \param dist must be a \ref distribution_t that has previously
    ///             been generated from a \ref distribution_builder_t via
    ///             distribution_build() and hasn't been turned back into a \ref
    ///             distribution_builder_t or destroyed since. It will be
    ///             consumed by this function and cannot be used again.
    UDIPE_NON_NULL_ARGS
    void distribution_pool_recycle(distribution_pool_t* pool,
                                   distribution_t* dist);

    /// Liberate a distribution pool
    ///
    /// This function must be called within the scope of with_logger().
    ///
    /// \param pool must be a \ref distribution_pool_t that was previously built
    ///             using distribution_pool_initialize() and hasn't been
    ///             destroyed since. It will be destroyed by this function and
    ///             cannot be used again.
    UDIPE_NON_NULL_ARGS
    void distribution_pool_finalize(distribution_pool_t* pool);


    // TODO: Add unit tests

#endif  // UDIPE_BUILD_BENCHMARKS