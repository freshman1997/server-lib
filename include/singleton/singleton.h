#ifndef __SINGLETON_H__
#define __SINGLETON_H__

namespace singleton 
{
    template<class T>
    T & Singleton()
    {
        static T instance;
        return instance;
    }
}

#endif
