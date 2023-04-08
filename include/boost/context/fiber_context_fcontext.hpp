
//          Copyright Oliver Kowalke 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_CONTEXT_FIBER_H
#define BOOST_CONTEXT_FIBER_H

#include <boost/context/detail/config.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <ostream>
#include <tuple>
#include <utility>

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/intrusive_ptr.hpp>

#if defined(BOOST_NO_CXX14_STD_EXCHANGE)
#include <boost/context/detail/exchange.hpp>
#endif
#if defined(BOOST_NO_CXX17_STD_INVOKE)
#include <boost/context/detail/invoke.hpp>
#endif
#include <boost/context/detail/disable_overload.hpp>
#include <boost/context/detail/exception.hpp>
#include <boost/context/detail/fcontext.hpp>
#include <boost/context/detail/tuple.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/flags.hpp>
#include <boost/context/preallocated.hpp>
#include <boost/context/segmented_stack.hpp>
#include <boost/context/stack_context.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_PREFIX
#endif

#if defined(__CET__) && defined(__unix__)
#  include <cet.h>
#  include <sys/mman.h>
#  define SHSTK_ENABLED (__CET__ & 0x2)
#  define BOOST_CONTEXT_SHADOW_STACK (SHSTK_ENABLED && SHADOW_STACK_SYSCALL)
#  define __NR_map_shadow_stack 451
#ifndef SHADOW_STACK_SET_TOKEN
#  define SHADOW_STACK_SET_TOKEN 0x1
#endif
#endif

#if defined(BOOST_MSVC)
# pragma warning(push)
# pragma warning(disable: 4702)
#endif

namespace boost {
namespace context {
namespace detail {

inline
transfer_t fiber_context_unwind( transfer_t t) {
    throw forced_unwind( t.fctx);
    return { nullptr, nullptr };
}

template< typename Rec >
transfer_t fiber_context_exit( transfer_t t) noexcept {
    Rec * rec = static_cast< Rec * >( t.data);
#if BOOST_CONTEXT_SHADOW_STACK
    // destory shadow stack
    std::size_t ss_size = *((unsigned long*)(reinterpret_cast< uintptr_t >( rec)- 16));
    long unsigned int ss_base = *((unsigned long*)(reinterpret_cast< uintptr_t >( rec)- 8));
    munmap((void *)ss_base, ss_size);
#endif
    // destroy context stack
    rec->deallocate();
    return { nullptr, nullptr };
}

template< typename Rec >
void fiber_context_entry( transfer_t t) noexcept {
    // transfer control structure to the context-stack
    Rec * rec = static_cast< Rec * >( t.data);
    BOOST_ASSERT( nullptr != t.fctx);
    BOOST_ASSERT( nullptr != rec);
    try {
        // jump back to `create_context()`
        t = jump_fcontext( t.fctx, nullptr);
        // start executing
        t.fctx = rec->run( t.fctx);
    } catch ( forced_unwind const& ex) {
        t = { ex.fctx, nullptr };
    }
    BOOST_ASSERT( nullptr != t.fctx);
    // destroy context-stack of `this`context on next context
    ontop_fcontext( t.fctx, rec, fiber_context_exit< Rec >);
    BOOST_ASSERT_MSG( false, "context already terminated");
}

template< typename Ctx, typename Fn >
transfer_t fiber_context_ontop( transfer_t t) {
    BOOST_ASSERT( nullptr != t.data);
    auto p = *static_cast< Fn * >( t.data);
    t.data = nullptr;
    // execute function, pass fiber_context via reference
    Ctx c = p( Ctx{ t.fctx } );
#if defined(BOOST_NO_CXX14_STD_EXCHANGE)
    return { exchange( c.fctx_, nullptr), nullptr };
#else
    return { std::exchange( c.fctx_, nullptr), nullptr };
#endif
}

template< typename Ctx, typename StackAlloc, typename Fn >
class fiber_context_record {
private:
    stack_context                                       sctx_;
    typename std::decay< StackAlloc >::type             salloc_;
    typename std::decay< Fn >::type                     fn_;

    static void destroy( fiber_context_record * p) noexcept {
        typename std::decay< StackAlloc >::type salloc = std::move( p->salloc_);
        stack_context sctx = p->sctx_;
        // deallocate fiber_context_record
        p->~fiber_context_record();
        // destroy stack with stack allocator
        salloc.deallocate( sctx);
    }

public:
    fiber_context_record( stack_context sctx, StackAlloc && salloc,
            Fn && fn) noexcept :
        sctx_( sctx),
        salloc_( std::forward< StackAlloc >( salloc)),
        fn_( std::forward< Fn >( fn) ) {
    }

    fiber_context_record( fiber_context_record const&) = delete;
    fiber_context_record & operator=( fiber_context_record const&) = delete;

    void deallocate() noexcept {
        destroy( this);
    }

    fcontext_t run( fcontext_t fctx) {
        // invoke context-function
#if defined(BOOST_NO_CXX17_STD_INVOKE)
        Ctx c = boost::context::detail::invoke( fn_, Ctx{ fctx } );
#else
        Ctx c = std::invoke( fn_, Ctx{ fctx } );
#endif
#if defined(BOOST_NO_CXX14_STD_EXCHANGE)
        return exchange( c.fctx_, nullptr);
#else
        return std::exchange( c.fctx_, nullptr);
#endif
    }
};

template< typename Record, typename StackAlloc, typename Fn >
fcontext_t create_fiber_context1( StackAlloc && salloc, Fn && fn) {
    auto sctx = salloc.allocate();
    // reserve space for control structure
	void * storage = reinterpret_cast< void * >(
			( reinterpret_cast< uintptr_t >( sctx.sp) - static_cast< uintptr_t >( sizeof( Record) ) )
            & ~static_cast< uintptr_t >( 0xff) );
    // placment new for control structure on context stack
    Record * record = new ( storage) Record{
            sctx, std::forward< StackAlloc >( salloc), std::forward< Fn >( fn) };
    // 64byte gab between control structure and stack top
    // should be 16byte aligned
    void * stack_top = reinterpret_cast< void * >(
            reinterpret_cast< uintptr_t >( storage) - static_cast< uintptr_t >( 64) );
    void * stack_bottom = reinterpret_cast< void * >(
            reinterpret_cast< uintptr_t >( sctx.sp) - static_cast< uintptr_t >( sctx.size) );
    // create fast-context
    const std::size_t size = reinterpret_cast< uintptr_t >( stack_top) - reinterpret_cast< uintptr_t >( stack_bottom);

#if BOOST_CONTEXT_SHADOW_STACK
    std::size_t ss_size = size >> 5;
    // align shadow stack to 8 bytes.
	ss_size = (ss_size + 7) & ~7;
    // Todo: shadow stack occupies at least 4KB
    ss_size = (ss_size > 4096) ? size : 4096;
    // create shadow stack
    void *ss_base = (void *)syscall(__NR_map_shadow_stack, 0, ss_size, SHADOW_STACK_SET_TOKEN);
    BOOST_ASSERT(ss_base != -1);
    unsigned long ss_sp = (unsigned long)ss_base + ss_size;
    /* pass the shadow stack pointer to make_fcontext
	 i.e., link the new shadow stack with the new fcontext
	 TODO should be a better way? */
    *((unsigned long*)(reinterpret_cast< uintptr_t >( stack_top)- 8)) = ss_sp;
    /* Todo: place shadow stack info in 64byte gap */
    *((unsigned long*)(reinterpret_cast< uintptr_t >( storage)- 8)) = (unsigned long) ss_base;
    *((unsigned long*)(reinterpret_cast< uintptr_t >( storage)- 16)) = ss_size;
#endif
    const fcontext_t fctx = make_fcontext( stack_top, size, & fiber_context_entry< Record >);
    BOOST_ASSERT( nullptr != fctx);
    // transfer control structure to context-stack
    return jump_fcontext( fctx, record).fctx;
}

template< typename Record, typename StackAlloc, typename Fn >
fcontext_t create_fiber_context2( preallocated palloc, StackAlloc && salloc, Fn && fn) {
    // reserve space for control structure
    void * storage = reinterpret_cast< void * >(
            ( reinterpret_cast< uintptr_t >( palloc.sp) - static_cast< uintptr_t >( sizeof( Record) ) )
            & ~ static_cast< uintptr_t >( 0xff) );
    // placment new for control structure on context-stack
    Record * record = new ( storage) Record{
            palloc.sctx, std::forward< StackAlloc >( salloc), std::forward< Fn >( fn) };
    // 64byte gab between control structure and stack top
    void * stack_top = reinterpret_cast< void * >(
            reinterpret_cast< uintptr_t >( storage) - static_cast< uintptr_t >( 64) );
    void * stack_bottom = reinterpret_cast< void * >(
            reinterpret_cast< uintptr_t >( palloc.sctx.sp) - static_cast< uintptr_t >( palloc.sctx.size) );
    // create fast-context
    const std::size_t size = reinterpret_cast< uintptr_t >( stack_top) - reinterpret_cast< uintptr_t >( stack_bottom);

#if BOOST_CONTEXT_SHADOW_STACK
    std::size_t ss_size = size >> 5;
    // align shadow stack to 8 bytes.
	ss_size = (ss_size + 7) & ~7;
    // Todo: shadow stack occupies at least 4KB
    ss_size = (ss_size > 4096) ? size : 4096;
    // create shadow stack
    void *ss_base = (void *)syscall(__NR_map_shadow_stack, 0, ss_size, SHADOW_STACK_SET_TOKEN);
    BOOST_ASSERT(ss_base != -1);
    unsigned long ss_sp = (unsigned long)ss_base + ss_size;
    /* pass the shadow stack pointer to make_fcontext
	 i.e., link the new shadow stack with the new fcontext
	 TODO should be a better way? */
    *((unsigned long*)(reinterpret_cast< uintptr_t >( stack_top)- 8)) = ss_sp;
    /* Todo: place shadow stack info in 64byte gap */
    *((unsigned long*)(reinterpret_cast< uintptr_t >( storage)- 8)) = (unsigned long) ss_base;
    *((unsigned long*)(reinterpret_cast< uintptr_t >( storage)- 16)) = ss_size;
#endif
    const fcontext_t fctx = make_fcontext( stack_top, size, & fiber_context_entry< Record >);
    BOOST_ASSERT( nullptr != fctx);
    // transfer control structure to context-stack
    return jump_fcontext( fctx, record).fctx;
}

}

class fiber_context {
private:
    template< typename Ctx, typename StackAlloc, typename Fn >
    friend class detail::fiber_context_record;

    template< typename Ctx, typename Fn >
    friend detail::transfer_t
    detail::fiber_context_ontop( detail::transfer_t);

    detail::fcontext_t  fctx_{ nullptr };

    fiber_context( detail::fcontext_t fctx) noexcept :
        fctx_{ fctx } {
    }

public:
    fiber_context() noexcept = default;

    template< typename Fn, typename = detail::disable_overload< fiber_context, Fn > >
    fiber_context( Fn && fn) :
        fiber_context{ std::allocator_arg, fixedsize_stack(), std::forward< Fn >( fn) } {
    }

    template< typename StackAlloc, typename Fn >
    fiber_context( std::allocator_arg_t, StackAlloc && salloc, Fn && fn) :
        fctx_{ detail::create_fiber_context1< detail::fiber_context_record< fiber_context, StackAlloc, Fn > >(
                std::forward< StackAlloc >( salloc), std::forward< Fn >( fn) ) } {
    }

    template< typename StackAlloc, typename Fn >
    fiber_context( std::allocator_arg_t, preallocated palloc, StackAlloc && salloc, Fn && fn) :
        fctx_{ detail::create_fiber_context2< detail::fiber_context_record< fiber_context, StackAlloc, Fn > >(
                palloc, std::forward< StackAlloc >( salloc), std::forward< Fn >( fn) ) } {
    }

#if defined(BOOST_USE_SEGMENTED_STACKS)
    template< typename Fn >
    fiber_context( std::allocator_arg_t, segmented_stack, Fn &&);

    template< typename StackAlloc, typename Fn >
    fiber_context( std::allocator_arg_t, preallocated, segmented_stack, Fn &&);
#endif

    ~fiber_context() {
        if ( BOOST_UNLIKELY( nullptr != fctx_) ) {
            detail::ontop_fcontext(
#if defined(BOOST_NO_CXX14_STD_EXCHANGE)
                    detail::exchange( fctx_, nullptr),
#else
                    std::exchange( fctx_, nullptr),
#endif
                   nullptr,
                   detail::fiber_context_unwind);
        }
    }

    fiber_context( fiber_context && other) noexcept {
        swap( other);
    }

    fiber_context & operator=( fiber_context && other) noexcept {
        if ( BOOST_LIKELY( this != & other) ) {
            fiber_context tmp = std::move( other);
            swap( tmp);
        }
        return * this;
    }

    fiber_context( fiber_context const& other) noexcept = delete;
    fiber_context & operator=( fiber_context const& other) noexcept = delete;

    fiber_context resume() && {
        BOOST_ASSERT( nullptr != fctx_);
        return { detail::jump_fcontext(
#if defined(BOOST_NO_CXX14_STD_EXCHANGE)
                    detail::exchange( fctx_, nullptr),
#else
                    std::exchange( fctx_, nullptr),
#endif
                    nullptr).fctx };
    }

    template< typename Fn >
    fiber_context resume_with( Fn && fn) && {
        BOOST_ASSERT( nullptr != fctx_);
        auto p = std::forward< Fn >( fn);
        return { detail::ontop_fcontext(
#if defined(BOOST_NO_CXX14_STD_EXCHANGE)
                    detail::exchange( fctx_, nullptr),
#else
                    std::exchange( fctx_, nullptr),
#endif
                    & p,
                    detail::fiber_context_ontop< fiber_context, decltype(p) >).fctx };
    }

    bool can_resume() noexcept {
        if ( ! empty() ) {
            // TODO:
            // *this has no owning thread
            // calling thread is the owning thread represented by *this
            return true;
        }
        return false;
    }

    explicit operator bool() const noexcept {
        return ! empty();
    }

    bool empty() const noexcept {
        return nullptr == fctx_;
    }

    void swap( fiber_context & other) noexcept {
        std::swap( fctx_, other.fctx_);
    }

    #if !defined(BOOST_EMBTC)
    
    template< typename charT, class traitsT >
    friend std::basic_ostream< charT, traitsT > &
    operator<<( std::basic_ostream< charT, traitsT > & os, fiber_context const& other) {
        if ( nullptr != other.fctx_) {
            return os << other.fctx_;
        } else {
            return os << "{not-a-context}";
        }
    }

    #else
    
    template< typename charT, class traitsT >
    friend std::basic_ostream< charT, traitsT > &
    operator<<( std::basic_ostream< charT, traitsT > & os, fiber_context const& other);

    #endif
};

#if defined(BOOST_EMBTC)

    template< typename charT, class traitsT >
    inline std::basic_ostream< charT, traitsT > &
    operator<<( std::basic_ostream< charT, traitsT > & os, fiber_context const& other) {
        if ( nullptr != other.fctx_) {
            return os << other.fctx_;
        } else {
            return os << "{not-a-context}";
        }
    }

#endif
    
inline
void swap( fiber_context & l, fiber_context & r) noexcept {
    l.swap( r);
}

}}

#if defined(BOOST_MSVC)
# pragma warning(pop)
#endif

#ifdef BOOST_HAS_ABI_HEADERS
# include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_CONTEXT_FIBER_H
