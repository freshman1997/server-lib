#ifndef __YUAN_BASE_OWNER_PTR_H__
#define __YUAN_BASE_OWNER_PTR_H__

namespace yuan::base
{

    template <typename Owner>
    auto owner_ptr(Owner &owner) noexcept -> decltype(owner.get())
    {
        return owner.get();
    }

    template <typename Owner>
    auto owner_ptr(const Owner &owner) noexcept -> decltype(owner.get())
    {
        return owner.get();
    }

} // namespace yuan::base

#endif
