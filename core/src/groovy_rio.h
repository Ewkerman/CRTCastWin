#pragma once

struct GMHandle;

/// Takes finished sends off groovymister's RIO send completion queue (see groovy_rio.cpp).
/// Call from the sender thread after gm_audio / gm_blit. Returns how many it removed, or -1 if the
/// queue already overflowed (RIO_CORRUPT_CQ).
long gm_rio_drain_sends(GMHandle *h);
