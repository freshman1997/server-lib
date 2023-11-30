#ifndef __SINGLETON_H__
#define __SINGLETON_H__

namespace singleton 
{
    template<class T>
    T & singleton()
    {
        static T instance;
        return instance;
    }
}

#endif
