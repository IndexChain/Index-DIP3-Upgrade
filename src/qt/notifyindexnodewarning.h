#ifndef ZCOIN_NOTIFYINDEXNODEWARNING_H
#define ZCOIN_NOTIFYINDEXNODEWARNING_H

class NotifyZnodeWarning
{
public:

    ~NotifyZnodeWarning();

    static void notify();
    static bool shouldShow();
    static bool nConsidered;
};

#endif //ZCOIN_NOTIFYINDEXNODEWARNING_H
