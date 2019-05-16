/*
 * If not stated otherwise in this file or this component's license file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/**
* @file fragmentcollector_hls.cpp
*
* Fragment Collect file includes class implementation for handling
* HLS streaming of AAMP player. 
* Various functionality like manifest download , fragment collection,
* DRM initialization , synchronizing audio / video media of the stream,
* trick play handling etc are handled in this file .
*
*/
#include "fragmentcollector_hls.h"
#include "_base64.h"
#include "base16.h"
#include <algorithm> // for std::min
#include <sys/time.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "priv_aamp.h"
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <openssl/sha.h>
#include <set>
#include "HlsDrmBase.h"

#ifdef AAMP_VANILLA_AES_SUPPORT
#include "aamp_aes.h"
#endif

/**
 * @addtogroup AAMP_COMMON_TYPES
 * @{
 */
//#define TRACE // compile-time optional noisy debug output

#define CHAR_CR 0x0d //!< '\r'
#define CHAR_LF 0x0a //!< '\n'
#define BOOLSTR(boolValue) (boolValue?"true":"false")
#define PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS (0.1f)
#define MAX_MANIFEST_DOWNLOAD_RETRY 3
#define MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS (6*1000)
#define MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS (500) //!< 500mSec
#define DRM_IV_LEN 16
#define MAX_LICENSE_ACQ_WAIT_TIME 12000  /*!< 12 secs Increase from 10 to 12 sec(DELIA-33528) */
#define MAX_SEQ_NUMBER_LAG_COUNT 50 /*!< Configured sequence number max count to avoid continuous looping for an edge case scenario, which leads crash due to hung */
#define MAX_SEQ_NUMBER_DIFF_FOR_SEQ_NUM_BASED_SYNC 2 /*!< Maximum difference in sequence number to sync tracks using sequence number.*/
#define DISCONTINUITY_DISCARD_TOLERANCE_SECONDS 30 /*!< Used by discontinuity handling logic to ensure both tracks have discontinuity tag around same area*/
#define MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_EVENT 5 /*!< Maximum playlist refresh count for discontinuity check for TSB/cDvr*/
#define MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_LIVE 1 /*!< Maximum playlist refresh count for discontinuity check for live without TSB*/

pthread_mutex_t gDrmMutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char gDeferredDrmMetaDataSha1Hash[DRM_SHA1_HASH_LEN]; /**< Sha1 hash of meta-data for deferred DRM license acquisition*/
static long long gDeferredDrmTime = 0;                     /**< Time at which deferred DRM license to be requested*/
static bool gDeferredDrmLicRequestPending = false;         /**< Indicates if deferred DRM request is pending*/
static bool gDeferredDrmLicTagUnderProcessing = false;     /**< Indicates if deferred DRM request tag is under processing*/

/**
* \struct	FormatMap
* \brief	FormatMap structure for stream codec/format information 
*/
struct FormatMap
{
	const char* codec;
	StreamOutputFormat format;
};

/// Variable initialization for various audio formats 
static const FormatMap audioFormatMap[AAMP_AUDIO_FORMAT_MAP_LEN] =
{
	{ "mp4a.40.2", FORMAT_AUDIO_ES_AAC },
	{ "mp4a.40.5", FORMAT_AUDIO_ES_AAC },
	{ "ac-3", FORMAT_AUDIO_ES_AC3 },
	{ "mp4a.a5", FORMAT_AUDIO_ES_AC3 },
	{ "ec-3", FORMAT_AUDIO_ES_EC3 },
	{ "ec+3", FORMAT_AUDIO_ES_ATMOS },
	{ "eac3", FORMAT_AUDIO_ES_EC3 }
};

/// Variable initialization for various video formats 
static const FormatMap videoFormatMap[AAMP_VIDEO_FORMAT_MAP_LEN] =
{
	{ "avc1.", FORMAT_VIDEO_ES_H264 },
	{ "hvc1.", FORMAT_VIDEO_ES_HEVC },
	{ "mpeg2v", FORMAT_VIDEO_ES_MPEG2 }//For testing.
};
/// Variable initialization for media profiler buckets 
static const ProfilerBucketType mediaTrackBucketTypes[AAMP_TRACK_COUNT] =
	{PROFILE_BUCKET_FRAGMENT_VIDEO, PROFILE_BUCKET_FRAGMENT_AUDIO};
/// Variable initialization for media decrypt buckets
static const ProfilerBucketType mediaTrackDecryptBucketTypes[AAMP_TRACK_COUNT] =
	{PROFILE_BUCKET_DECRYPT_VIDEO, PROFILE_BUCKET_DECRYPT_AUDIO};
/**
 * @}
 */

/**
 * @addtogroup AAMP_COMMON_API
 * @{
 */

/***************************************************************************
* @fn startswith
* @brief Function to check if string is start of main string
*        
* @param[in]  pstring Main string   
* @param[in]  prefix  Sub string to check
*
* @return bool true/false
***************************************************************************/
static bool startswith(char **pstring, const char *prefix)
{
	char *string = *pstring;
	for (;;)
	{
		char c = *prefix++;
		if (c == 0)
		{
			*pstring = string;
			return true;
		}
		if (*string++ != c)
		{
			return false;
		}
	}
}

/***************************************************************************
* @fn AttributeNameMatch
* @brief Function to check if attribute name matches
*        
* @param[in] attrNamePtr            Attribute Name pointer   
* @param[in] targetAttrNameCString  String to compare
*
* @return bool true/false
***************************************************************************/
static bool AttributeNameMatch(const char *attrNamePtr, const char *targetAttrNameCString)
{
	while (*targetAttrNameCString)
	{
		if (*targetAttrNameCString++ != *attrNamePtr++)
		{
			return false;
		}
	}
	return *attrNamePtr == '=';
}

/***************************************************************************
* @fn SubStringMatch
* @brief Function to check if substring present in main string 
*        
* @param[in] srcStart  Start of string pointer  
* @param[in] srcFin    End of string pointer (length = srcFin - srcStart)
* @param[in] cstring   String to compare
*
* @return bool true/false
***************************************************************************/
static bool SubStringMatch(const char *srcStart, const char *srcFin, const char *cstring)
{
	while (srcStart <= srcFin)
	{
		char c = *cstring++;
		if (c == 0)
		{
			return true;
		}
		if (*srcStart != c)
		{
			break;
		}
		srcStart++;
	}
	return false;
}

/***************************************************************************
* @fn GetAttributeValueString
* @brief Convert quoted string to NUL-terminated C-string; modifies string in place 
*        
* @param[in]  valuePtr Quoted string  
* @param[in]  fin      End of string pointer 
*
* @return char * pointed to first character of NUL-termianted string
***************************************************************************/
static char * GetAttributeValueString(char *valuePtr, char *fin)
{
	if (*valuePtr == '\"')
	{
		assert(*valuePtr == '\"');
		valuePtr++; // skip begin-quote
		fin--;
		assert(*fin == '\"');
		size_t len = fin - valuePtr;
		valuePtr[len] = 0x00; // replace end-quote with NUL-terminator
	}
	else if (strcmp(valuePtr, "NONE") == 0)
	{
		// patch for http://cilhlsvod.akamaized.net/i/506629/MP4/demo3/abcd123/,cea708test,.mp4.csmil/master.m3u8
		// and watchable content; these use CLOSED_CAPTION=NONE
	}
	else
	{
		logprintf("WARNING: GetAttributeValueString(%s)\n", valuePtr );
	}
	return valuePtr;
}

/***************************************************************************
* @fn ParseKeyAttributeCallback
* @brief Callback function to decode Key and attribute
*        
* @param[in]  attrName   Input string  
* @param[in]  delimEqual Delimiter string
* @param[in]  fin        String end pointer
* @param[out] arg        TrackState pointer for storage
*
* @return void
***************************************************************************/
static void ParseKeyAttributeCallback(char *attrName, char *delimEqual, char *fin, void* arg)
{
	TrackState *ts = (TrackState *)arg;
	char *valuePtr = delimEqual + 1;
	if (AttributeNameMatch(attrName, "METHOD"))
	{
		if (SubStringMatch(valuePtr, fin, "NONE"))
		{ // used by DAI
			if(ts->fragmentEncrypted)
			{
				if (!ts->mIndexingInProgress)
				{
					logprintf("Track %s encrypted to clear \n", ts->name);
				}
				ts->fragmentEncrypted = false;
				ts->UpdateDrmCMSha1Hash(NULL);
			}
		}
		else if (SubStringMatch(valuePtr, fin, "AES-128"))
		{
			if(!ts->fragmentEncrypted)
			{
				if (!ts->mIndexingInProgress)
				{
					AAMPLOG_WARN("Track %s clear to encrypted \n", ts->name);
				}
				ts->fragmentEncrypted = true;
			}
			ts->mDrmInfo.method = eMETHOD_AES_128;
		}
		else if (SubStringMatch(valuePtr, fin, "SAMPLE-AES"))
		{
			aamp_Error("SAMPLE-AES unsupported");
		}
		else
		{
			aamp_Error("unsupported METHOD");
		}
	}
	else if (AttributeNameMatch(attrName, "URI"))
	{
		char *uri = GetAttributeValueString(valuePtr, fin);
		if (ts->mDrmInfo.uri)
		{
			free(ts->mDrmInfo.uri);
		}
		ts->mDrmInfo.uri = strdup(uri);
		//		const char *rkEnd;
		//		const char *rkStart = FindUriAttr(this->mDrmInfo.uri, "EncryptedRK", &rkEnd);
		//		if (rkStart)
		//		{ // ParseHexData(mDrmInfo.encryptedRotationKey, 16, rkStart, rkEnd);
		//			aamp_Error("EncryptedRK unsupported");
		//		}
	}
	else if (AttributeNameMatch(attrName, "IV"))
	{ // 16 bytes
		const char *srcPtr = valuePtr;
		assert(srcPtr[0] == '0');
		assert(srcPtr[1] == 'x');
		srcPtr += 2;
		ts->UpdateDrmIV(srcPtr);
	}
	else if (AttributeNameMatch(attrName, "CMSha1Hash"))
	{ // 20 bytes; Metadata Hash.
		assert(valuePtr[0] == '0');
		assert(valuePtr[1] == 'x');
		ts->UpdateDrmCMSha1Hash(valuePtr+2);
	}
}

/***************************************************************************
* @fn ParseStreamInfCallback
* @brief Callback function to extract stream tag attributes
*        
* @param[in]  attrName   Input string  
* @param[in]  delimEqual Delimiter string
* @param[in]  fin        String end pointer
* @param[out] arg        StreamAbstractionAAMP_HLS pointer for storage
*
* @return void
***************************************************************************/
static void ParseStreamInfCallback(char *attrName, char *delimEqual, char *fin, void* arg)
{
	StreamAbstractionAAMP_HLS *context = (StreamAbstractionAAMP_HLS *) arg;
	char *valuePtr = delimEqual + 1;
	HlsStreamInfo *streamInfo = &context->streamInfo[context->GetProfileCount()];
	if (AttributeNameMatch(attrName, "URI"))
	{
		streamInfo->uri = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "BANDWIDTH"))
	{
		streamInfo->bandwidthBitsPerSecond = atol(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "PROGRAM-ID"))
	{
		streamInfo->program_id = atol(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "AUDIO"))
	{
		streamInfo->audio = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "CODECS"))
	{
		streamInfo->codecs = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "RESOLUTION"))
	{
		sscanf(delimEqual + 1, "%dx%d", &streamInfo->resolution.width, &streamInfo->resolution.height);
	}
	// following are rarely present
	else if (AttributeNameMatch(attrName, "AVERAGE-BANDWIDTH"))
	{
		streamInfo->averageBandwidth = atol(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "FRAME-RATE"))
	{
		streamInfo->frameRate = atof(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "CLOSED-CAPTIONS"))
	{
		streamInfo->closedCaptions = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "SUBTITLES"))
	{
		streamInfo->subtitles = GetAttributeValueString(valuePtr, fin);
	}
	else 
	{
		AAMPLOG_INFO("unknown stream inf attribute %s\n", attrName);
	}
}

/***************************************************************************
* @fn ParseMediaAttributeCallback
* @brief Callback function to extract media tag attributes
*		 
* @param[in]  attrName   Input string	
* @param[in]  delimEqual Delimiter string
* @param[in]  fin        String end pointer
* @param[out] arg        StreamAbstractionAAMP_HLS pointer for storage
*
* @return void
***************************************************************************/
static void ParseMediaAttributeCallback(char *attrName, char *delimEqual, char *fin, void *arg)
{
	StreamAbstractionAAMP_HLS *context = (StreamAbstractionAAMP_HLS *) arg;
	char *valuePtr = delimEqual + 1;
	struct MediaInfo *mediaInfo = &context->mediaInfo[context->mediaCount];
	/*
	#EXT - X - MEDIA:TYPE = AUDIO, GROUP - ID = "g117600", NAME = "English", LANGUAGE = "en", DEFAULT = YES, AUTOSELECT = YES
	#EXT - X - MEDIA:TYPE = AUDIO, GROUP - ID = "g117600", NAME = "Spanish", LANGUAGE = "es", URI = "HBOHD_HD_NAT_15152_0_5939026565177792163/format-hls-track-sap-bandwidth-117600-repid-root_audio103.m3u8"
	*/
	if (AttributeNameMatch(attrName, "TYPE"))
	{
		if (SubStringMatch(valuePtr, fin, "AUDIO"))
		{
			mediaInfo->type = eMEDIATYPE_AUDIO;
		}
		else if (SubStringMatch(valuePtr, fin, "VIDEO"))
		{
			mediaInfo->type = eMEDIATYPE_VIDEO;
		}
	}
	else if (AttributeNameMatch(attrName, "GROUP-ID"))
	{
		mediaInfo->group_id = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "NAME"))
	{
		mediaInfo->name = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "LANGUAGE"))
	{
		mediaInfo->language = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "AUTOSELECT"))
	{
		if (SubStringMatch(valuePtr, fin, "YES"))
		{
			mediaInfo->autoselect = true;
		}
	}
	else if (AttributeNameMatch(attrName, "DEFAULT"))
	{
		if (SubStringMatch(valuePtr, fin, "YES"))
		{
			mediaInfo->isDefault = true;
		}
	}
	else if (AttributeNameMatch(attrName, "URI"))
	{
		mediaInfo->uri = GetAttributeValueString(valuePtr, fin);
	}
	// rarely present
	else if (AttributeNameMatch(attrName, "CHANNELS"))
	{
		mediaInfo->channels = atoi(valuePtr);
	}
	else if (AttributeNameMatch(attrName, "INSTREAM-ID"))
	{
		mediaInfo->instreamID = GetAttributeValueString(valuePtr, fin);
	}
	else if (AttributeNameMatch(attrName, "FORCED"))
	{
		if (SubStringMatch(valuePtr, fin, "YES"))
		{
			mediaInfo->forced = true;
		}
	}
	else
	{
		logprintf("unk MEDIA attr %s\n", attrName);
	}
}

/***************************************************************************
* @fn mystrpbrk
* @brief Function to extract string pointer afer LF
*		 
* @param[in] ptr  Input string	
*
* @return char * - Next string pointer
***************************************************************************/
static char *mystrpbrk(char *ptr)
{ // lines are terminated by either a single LF character or a CR character followed by an LF character
	char *next = NULL;
	char *fin = strchr(ptr, CHAR_LF);
	if (fin)
	{
		next = fin + 1;
		if (fin > ptr && fin[-1] == CHAR_CR)
		{
			fin--;
		}
		*fin = 0x00;
	}
	return next;
}

/**
* @param attrName pointer to HLS Attribute List, as a NUL-terminated CString
*/
/***************************************************************************
* @fn ParseAttrList
* @brief Function to parse attribute list from tag string 
*		 
* @param[in] attrName  Input string	
* @param[in] cb        Callback function to store parsed attributes
* @param[in] context   Void pointer context
*
* @return void
***************************************************************************/
static void ParseAttrList(char *attrName, void(*cb)(char *attrName, char *delim, char *fin, void *context), void *context)
{
	while (*attrName)
	{
		while (*attrName == ' ')
		{ // strip whitespace
			attrName++;
		}
		char *delimEqual = attrName;
		while (*delimEqual != '=')
		{ // An AttributeName is an unquoted string containing characters from the set [A..Z] and '-'
			char c = *delimEqual++;
			(void)c;
			// assert((c >= 'A' && c <= 'Z') || c == '-'); // breaks when CMSha1Hash used
		}
		char *fin = delimEqual;
		bool inQuote = false;
		for (;;)
		{
			char c = *fin;
			if (c == 0)
			{
				break;
			}
			else if (c == '\"')
			{
				if (inQuote)
				{
					inQuote = false;
					fin++;
					break;
				}
				else
				{
					inQuote = true;
				}
			}
			else if (c == ',' && !inQuote)
			{
				break;
			}
			fin++;
		}
		cb(attrName, delimEqual, fin, context);
		if (*fin == ',')
		{
			fin++;
		}
		attrName = fin;
	}
}

static bool ParseTimeFromProgramDateTime(const char* ptr, struct timeval &programDateTimeVal )
{
	bool retVal = false;
	struct tm timeinfo;
	int ms = 0;
	memset(&timeinfo, 0, sizeof(timeinfo));
	/* discarding timezone assuming audio and video tracks has same timezone and we use this time only for synchronization*/
	int ret = sscanf(ptr, "%d-%d-%dT%d:%d:%d.%d", &timeinfo.tm_year, &timeinfo.tm_mon,
			&timeinfo.tm_mday, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec, &ms);
	if (ret >= 6)
	{
		timeinfo.tm_year -= 1900;
		programDateTimeVal.tv_sec = mktime(&timeinfo);
		programDateTimeVal.tv_usec = ms * 1000;
		retVal = true;
	}
	else
	{
		logprintf("Parse error on DATE-TIME: %*s ret = %d\n", 30, ptr, ret);
	}
	return retVal;
}

/***************************************************************************
* @fn TrackPLDownloader
* @brief Thread function for download 
*		 
* @param[in] arg void ptr , thread argument
*
* @return void ptr
***************************************************************************/
static void * TrackPLDownloader(void *arg)
{
	TrackState* ts = (TrackState*)arg;
	if(aamp_pthread_setname(pthread_self(), "aampAudPL"))
	{
		logprintf("%s:%d: aamp_pthread_setname failed\n", __FUNCTION__, __LINE__);
	}
	ts->FetchPlaylist();
	return NULL;
}

/***************************************************************************
* @fn ParseMainManifest
* @brief Function to parse main manifest 
*		 
* @param ptr[in] Manifest file content string
*	
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::ParseMainManifest(char *ptr)
{
	mAbrManager.clearProfiles();
	while (ptr)
	{
		char *next = mystrpbrk(ptr);
		if (*ptr)
		{
			if (startswith(&ptr, "#EXT"))
			{
				if (startswith(&ptr, "-X-I-FRAME-STREAM-INF:"))
				{
					HlsStreamInfo *streamInfo = &this->streamInfo[GetProfileCount()];
					memset(streamInfo, 0, sizeof(*streamInfo));
					ParseAttrList(ptr, ParseStreamInfCallback, this);
					if (streamInfo->uri == NULL)
					{ // uri on following line
						streamInfo->uri = next;
						next = mystrpbrk(next);
					}
					streamInfo->isIframeTrack = true;
					mAbrManager.addProfile({
						streamInfo->isIframeTrack,
						streamInfo->bandwidthBitsPerSecond,
						streamInfo->resolution.width,
						streamInfo->resolution.height,
					});
				}
				else if (startswith(&ptr, "-X-STREAM-INF:"))
				{
					struct HlsStreamInfo *streamInfo = &this->streamInfo[GetProfileCount()];
					memset(streamInfo, 0, sizeof(HlsStreamInfo));
					ParseAttrList(ptr, ParseStreamInfCallback, this);
					if (streamInfo->uri == NULL)
					{ // uri on following line
						streamInfo->uri = next;
						next = mystrpbrk(next);
					}
					mAbrManager.addProfile({
						streamInfo->isIframeTrack,
						streamInfo->bandwidthBitsPerSecond,
						streamInfo->resolution.width,
						streamInfo->resolution.height
					});
				}
				else if (startswith(&ptr, "-X-MEDIA:"))
				{
					memset(&this->mediaInfo[this->mediaCount], 0, sizeof(MediaInfo));
					ParseAttrList(ptr, ParseMediaAttributeCallback, this);
					this->mediaCount++;
				}
				else if (startswith(&ptr, "-X-VERSION:"))
				{
					// followed by integer
				}
				else if (startswith(&ptr, "-X-INDEPENDENT-SEGMENTS"))
				{
					// followed by integer
				}
				else if (startswith(&ptr, "-X-FAXS-CM"))
				{ // not needed - present in playlist
					hasDrm = true;
				}
				else if (startswith(&ptr, "M3U"))
				{
				}
				else if (startswith(&ptr, "-X-CONTENT-IDENTIFIER"))
				{
				}
				else if (startswith(&ptr, "-X-FOG"))
				{
				}
				else if (startswith(&ptr, "-X-XCAL-CONTENTMETADATA"))
				{ // placeholder for new Super8 DRM Agnostic Metadata
				}
				else if (startswith(&ptr, "-NOM-I-FRAME-DISTANCE"))
				{ // placeholder for nominal distance between IFrames
				}
				else if (startswith(&ptr, "-X-ADVERTISING"))
				{ // placeholder for advertising zone for linear (soon to be deprecated)
				}
				else 
				{
					std::string unknowTag= ptr;
					AAMPLOG_INFO("***unknown tag:%s\n", unknowTag.substr(0,24).c_str());
				}
			}
		}
		ptr = next;
	}
	UpdateIframeTracks();
} // ParseMainManifest

#ifdef AAMP_REWIND_PLAYLIST_SUPPORTED
static char *RewindPlaylist(TrackState *trackState)
{ // TODO: deprecate?
  // Right now, disabled under a compile-time macro
	char *ptr = trackState->fragmentURI;
	char *rc = NULL;
	while (ptr > trackState->playlist.ptr)
	{
		ptr--;
		/*Undo NULL char insertions*/
		if (*ptr == 0x00)
		{
			if (*(ptr + 1) == CHAR_LF)
			{
				*ptr = CHAR_CR;
			}
			else
			{
				*ptr = CHAR_LF;
			}

		}
		char * start = ptr;
		if (startswith(&start, "#EXTINF:"))
		{
			if (0 < trackState->playlistPosition)
			{
				trackState->playlistPosition -= atof(start);
				trackState->nextMediaSequenceNumber--;
#ifdef TRACE
				logprintf("Rew - EXTINF - trackState->playlistPosition updated to %f\n", trackState->playlistPosition);
#endif
				if (trackState->playlistPosition < trackState->playTarget)
				{
					rc = ptr;
					break;
				}
			}
			else
			{
#ifdef TRACE
				logprintf("Rew - EXTINF - trackState->playlistPosition reset to %f\n", trackState->playlistPosition);
#endif
				trackState->playlistPosition = -1;
				rc = ptr;
				break;
			}
		}
	}
	return rc;
}
#endif

/***************************************************************************
* @fn GetFragmentUriFromIndex
* @brief Function to get fragment URI from index count
*		 
* @return String fragment URI pointer
***************************************************************************/
char *TrackState::GetFragmentUriFromIndex()
{
	char * uri = NULL;
	const IndexNode *index = (IndexNode *) this->index.ptr;
	const IndexNode *idxNode = NULL;
	int idx;
	if (context->rate > 0)
	{

		const IndexNode *lastIndexNode = &index[indexCount - 1];
		double seekWindowEnd = lastIndexNode->completionTimeSecondsFromStart - aamp->mLiveOffset; 
		if (aamp->IsLive() && playTarget > seekWindowEnd)
		{
			logprintf("%s - rate - %f playTarget(%f) > seekWindowEnd(%f), forcing EOS\n",
                                __FUNCTION__, context->rate, playTarget, seekWindowEnd);
			return NULL;
		}

		if (currentIdx == -1)
		{ // search forward from beginning
			currentIdx = 0;
		}
		for (idx = currentIdx; idx < indexCount; idx++)
		{ // search in direction until out-of-bounds
			const IndexNode *node = &index[idx];
			//logprintf("%s rate %f completionTimeSecondsFromStart %f playTarget %f\n",__FUNCTION__, rate, node->completionTimeSecondsFromStart, playTarget);
			if (node->completionTimeSecondsFromStart >= playTarget)
			{ // found target iframe
#ifdef TRACE
				logprintf("%s Found node - rate %f completionTimeSecondsFromStart %f playTarget %f\n", __FUNCTION__,
				        context->rate, node->completionTimeSecondsFromStart, playTarget);
#endif
				idxNode = node;
				break;
			}
		}
	}
	else
	{
		if (-1 == currentIdx)
		{ // search backward from end
			currentIdx = indexCount - 1;
		}
		for (idx = currentIdx; idx >= 0; idx--)
		{ // search in direction until out-of-bounds
			const IndexNode *node = &index[idx];
			//logprintf("%s rate %f completionTimeSecondsFromStart %f playTarget %f\n",__FUNCTION__, rate, node->completionTimeSecondsFromStart, playTarget);
			if (node->completionTimeSecondsFromStart <= playTarget)
			{ // found target iframe
#ifdef TRACE
				logprintf("%s Found node - rate %f completionTimeSecondsFromStart %f playTarget %f\n",
						__FUNCTION__, context->rate, node->completionTimeSecondsFromStart, playTarget);
#endif

				idxNode = node;
				break;
			}
		}
	}
	if (idxNode)
	{
		currentIdx = idx;
		byteRangeOffset = 0;
		byteRangeLength = 0;
		//logprintf("%s fragmentinfo %s\n", __FUNCTION__, idxNode->pFragmentInfo);
		const char *fragmentInfo = idxNode->pFragmentInfo;
		fragmentDurationSeconds = idxNode->completionTimeSecondsFromStart;
		if (idx > 0)
		{
			fragmentDurationSeconds -= index[idx - 1].completionTimeSecondsFromStart;
		}
		while (fragmentInfo[0] == '#')
		{
			if (!memcmp(fragmentInfo, "#EXT-X-BYTERANGE:", 17))
			{
				char temp[1024];
				const char * end = fragmentInfo;
				while (end[0] != CHAR_LF)
				{
					end++;
				}
				int len = end - fragmentInfo;
				assert(len < 1024);
				strncpy(temp, fragmentInfo + 17, len);
				temp[1023] = 0x00;
				char * offsetDelim = strchr(temp, '@'); // optional
				if (offsetDelim)
				{
					*offsetDelim++ = 0x00;
					byteRangeOffset = atoi(offsetDelim);
				}
				byteRangeLength = atoi(temp);
			}
			/*Skip to next line*/
			while (fragmentInfo[0] != CHAR_LF)
			{
				fragmentInfo++;
			}
			fragmentInfo++;
		}
		const char *urlEnd = strchr(fragmentInfo, CHAR_LF);
		if (urlEnd)
		{
			if (*(urlEnd - 1) == CHAR_CR)
			{
				urlEnd--;
			}
			int urlLen = urlEnd - fragmentInfo;
			uri = fragmentURIFromIndex;
			assert(urlLen < MAX_URI_LENGTH);
			memcpy(uri, fragmentInfo, urlLen);
			uri[urlLen] = 0;
			//logprintf("%s - parsed uri %s\n", __FUNCTION__, uri);
		}
		else
		{
			logprintf("%s - unable to find end\n", __FUNCTION__);
		}
		if (-1 == idxNode->drmMetadataIdx)
		{
			fragmentEncrypted = false;
		}
		else
		{
			fragmentEncrypted = true;
			mDrmMetaDataIndexPosition = idxNode->drmMetadataIdx;
		}
	}
	else
	{
		logprintf("%s - Couldn't find node - rate %f playTarget %f\n",
				__FUNCTION__, context->rate, playTarget);
	}
	return uri;
}

/***************************************************************************
* @fn GetNextFragmentUriFromPlaylist
* @brief Function to get next fragment URI from playlist based on playtarget
* @param ignoreDiscontinuity Ignore discontinuity
* @return string fragment URI pointer
***************************************************************************/
char *TrackState::GetNextFragmentUriFromPlaylist(bool ignoreDiscontinuity)
{
	char *ptr = fragmentURI;
	char *rc = NULL;
	int byteRangeLength = 0; // default, when optional byterange offset is left unspecified
	int byteRangeOffset = 0;
	bool discontinuity = false;
	const char* programDateTime = NULL;

	traceprintf ("GetNextFragmentUriFromPlaylist : playTarget %f playlistPosition %f fragmentURI %p\n", playTarget, playlistPosition, fragmentURI);
	if (playTarget < 0)
	{
		logprintf("%s - invalid playTarget %f \n", __FUNCTION__, playTarget);
		playTarget = 0;
		//return fragmentURI; // leads to buffer overrun/crash
	}
	if (playlistPosition == playTarget)
	{
		//logprintf("[PLAYLIST_POSITION==PLAY_TARGET]\n");
		return fragmentURI;
	}
	if (playlistPosition != -1)
	{ // already presenting - skip past previous segment
		//logprintf("[PLAYLIST_POSITION!= -1]\n");
		ptr += strlen(fragmentURI) + 1;
	}
#ifdef AAMP_REWIND_PLAYLIST_SUPPORTED
	if (playlistPosition > playTarget)
	{
		logprintf("%s - playlistPosition[%f] > playTarget[%f] [REWIND]\n", __FUNCTION__, playlistPosition, playTarget);
		ptr = RewindPlaylist(this);
	}
#else
	if ((playlistPosition > playTarget) && (fragmentDurationSeconds > PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS) &&
		((playlistPosition - playTarget) > fragmentDurationSeconds))
	{
		logprintf("%s - playlistPosition[%f] > playTarget[%f] more than last fragmentDurationSeconds[%f]\n",
					__FUNCTION__, playlistPosition, playTarget, fragmentDurationSeconds);
	}
#endif
	//logprintf("%s: before loop, ptr = %p fragmentURI %p\n", __FUNCTION__, ptr, fragmentURI);
	while (ptr)
	{
		char *next = mystrpbrk(ptr);
		//logprintf("ptr %s next %.*s\n", ptr, 10, next);
		if (*ptr)
		{
			if (startswith(&ptr, "#EXT"))
			{ // tags begins with #EXT
				if (startswith(&ptr, "M3U"))
				{ // "Extended M3U file" - always first line
				}
				else if (startswith(&ptr, "INF:"))
				{// preceeds each advertised fragment in a playlist
					if (-1 != playlistPosition)
					{
						playlistPosition += fragmentDurationSeconds;
					}
					else
					{
						playlistPosition = 0;
					}
					fragmentDurationSeconds = atof(ptr);
#ifdef TRACE
					logprintf("Next - EXTINF - playlistPosition updated to %f\n", playlistPosition);
					// optionally followed by human-readable title
#endif
				}
				else if (startswith(&ptr, "-X-BYTERANGE:"))
				{
					char  temp[1024];
					strncpy(temp, ptr, 1023);
					temp[1023] = 0x00;
					char * offsetDelim = strchr(temp, '@'); // optional
					if (offsetDelim)
					{
						*offsetDelim++ = 0x00;
						byteRangeOffset = atoi(offsetDelim);
					}
					byteRangeLength = atoi(temp);
				}
				else if (startswith(&ptr, "-X-TARGETDURATION:"))
				{ // max media segment duration; required; appears once
					targetDurationSeconds = atof(ptr);
				}
				else if (startswith(&ptr, "-X-MEDIA-SEQUENCE:"))
				{// first media URI's unique integer sequence number
					nextMediaSequenceNumber = atoll(ptr);
				}
				else if (startswith(&ptr, "-X-KEY:"))
				{ // identifies licensing server to contact for authentication
					ParseAttrList(ptr, ParseKeyAttributeCallback, this);
				}
				else if (startswith(&ptr, "-X-PROGRAM-DATE-TIME:"))
				{ // associates following media URI with absolute date/time
					// if used, should supplement any EXT-X-DISCONTINUITY tags
					AAMPLOG_TRACE("Got EXT-X-PROGRAM-DATE-TIME: %s \n", ptr);
					if (context->mNumberOfTracks > 1)
					{
						programDateTime = ptr;
						// The first X-PROGRAM-DATE-TIME tag holds the start time for each track
						if (startTimeForPlaylistSync.tv_sec == 0 && startTimeForPlaylistSync.tv_usec == 0)
						{
							/* discarding timezone assuming audio and video tracks has same timezone and we use this time only for synchronization*/
							bool ret = ParseTimeFromProgramDateTime(ptr, startTimeForPlaylistSync);
							if (ret)
							{
								AAMPLOG_TRACE("DATE-TIME: %s startTime updated to %ld.%06ld\n",
										ptr ,startTimeForPlaylistSync.tv_sec,
										(long)startTimeForPlaylistSync.tv_usec);
							}
						}
					}
				}
				else if (startswith(&ptr, "-X-ALLOW-CACHE:"))
				{ // YES or NO - authorizes client to cache segments for later replay
					if (startswith(&ptr, "YES"))
					{
						context->allowsCache = true;
					}
					else if (startswith(&ptr, "NO"))
					{
						context->allowsCache = false;
					}
					else
					{
						aamp_Error("unknown ALLOW-CACHE setting");
					}
				}
				else if (startswith(&ptr, "-X-PLAYLIST-TYPE:"))
				{
					//PlaylistType is handled during indexing.
				}
				else if (startswith(&ptr, "-X-ENDLIST"))
				{ // indicates that no more media segments are available
					logprintf("#EXT-X-ENDLIST\n");
					context->hasEndListTag = true;
				}
				else if (startswith(&ptr, "-X-DISCONTINUITY"))
				{
					discontinuity = true;
				}
				else if (startswith(&ptr, "-X-I-FRAMES-ONLY"))
				{
					logprintf("#EXT-X-I-FRAMES-ONLY\n");
				}
				else if (startswith(&ptr, "-X-VERSION:"))
				{
					int version = atoi(ptr);
					(void)version;
				}
				// custom tags follow:
				else if (startswith(&ptr, "-X-FAXS-CM:"))
				{
					//DRM meta data is stored during indexing.
				}
				else if (startswith(&ptr, "-X-FAXS-PACKAGINGCERT"))
				{
				}
				else if (startswith(&ptr, "-X-FAXS-SIGNATURE"))
				{
				}
				else if (startswith(&ptr, "-X-CUE"))
				{
				}
				else if (startswith(&ptr, "-X-CM-SEQUENCE"))
				{
				}
				else if (startswith(&ptr, "-X-MARKER"))
				{
				}
				else if (startswith(&ptr, "-X-MAP"))
				{
				}
				else if (startswith(&ptr, "-X-MEDIA-TIME"))
				{
				}
				else if (startswith(&ptr, "-X-END-TOP-TAGS"))
				{
				}
				else if (startswith(&ptr, "-X-CONTENT-IDENTIFIER"))
				{
				}
				else if (startswith(&ptr, "-X-TRICKMODE-RESTRICTION"))
				{
				}
				else if (startswith(&ptr, "-X-INDEPENDENT-SEGMENTS"))
				{
				}
				else if (startswith(&ptr, "-X-BITRATE"))
				{
				}
				else if (startswith(&ptr, "-X-FOG"))
				{
				}
				else if (startswith(&ptr, "-X-START:"))
				{ // sling
					//"TIME-OFFSET=2.336, PRECISE=YES"
				}
				else if (startswith(&ptr, "-X-XCAL-CONTENTMETADATA"))
				{ // placeholder for new Super8 DRM Agnostic Metadata
				}
				else if (startswith(&ptr, "-NOM-I-FRAME-DISTANCE"))
				{ // placeholder for nominal distance between IFrames
				}
				else if (startswith(&ptr, "-X-ADVERTISING"))
				{ // placeholder for advertising zone for linear (soon to be deprecated)
				}
				else 
				{
					std::string unknowTag= ptr;
					AAMPLOG_INFO("***unknown tag:%s\n", unknowTag.substr(0,24).c_str());	
				}
			}
			else if (*ptr == '#')
			{ // all other lines beginning with # are comments
			}
			else if (*ptr == '\0')
			{ // skip inserted null char
			}
			else
			{ // URI
				nextMediaSequenceNumber++;
				if ((playlistPosition >= playTarget) || ((playTarget - playlistPosition) < PLAYLIST_TIME_DIFF_THRESHOLD_SECONDS))
				{
					//logprintf("Return fragment %s playlistPosition %f playTarget %f\n", ptr, playlistPosition, playTarget);
					this->byteRangeOffset = byteRangeOffset;
					this->byteRangeLength = byteRangeLength;
					if (discontinuity)
					{
						if (!ignoreDiscontinuity)
						{
							logprintf("%s:%d #EXT-X-DISCONTINUITY in track[%d] playTarget %f total mCulledSeconds %f\n", __FUNCTION__, __LINE__, type, playTarget, mCulledSeconds);
							TrackType otherType = (type == eTRACK_VIDEO)? eTRACK_AUDIO: eTRACK_VIDEO;
							TrackState *other = context->trackState[otherType];
							if (other->enabled)
							{
								double diff;
								double position;
								double playPosition = playTarget - mCulledSeconds;
								if (!programDateTime)
								{
									position = playPosition;
								}
								else
								{
									struct timeval programDateTimeVal;
									bool ret = ParseTimeFromProgramDateTime(programDateTime, programDateTimeVal);
									if (ret)
									{
										AAMPLOG_TRACE("DATE-TIME: %s startTime updated to %ld.%06ld\n",
												ptr ,programDateTimeVal.tv_sec,
												(long)programDateTimeVal.tv_usec);
									}
									position = programDateTimeVal.tv_sec + (double)programDateTimeVal.tv_usec/1000000;
									logprintf("%s:%d [%s] Discontinuity - position from program-date-time %f\n", __FUNCTION__, __LINE__, name, position);
								}
								if (!other->HasDiscontinuityAroundPosition(position, (NULL != programDateTime), diff, playPosition))
								{
									logprintf("%s:%d [%s] Ignoring discontinuity as %s track does not have discontinuity\n", __FUNCTION__, __LINE__, name, other->name);
									discontinuity = false;
								}
								else if (programDateTime)
								{
									logprintf("%s:%d [%s] diff %f \n", __FUNCTION__, __LINE__, name, diff);
									/*If other track's discontinuity is in advanced position, diff is positive*/
									if (diff > fragmentDurationSeconds/2 )
									{
										/*Skip fragment*/
										logprintf("%s:%d [%s] Discontinuity - other track's discontinuity time greater by %f. updating playTarget %f to %f\n",
												__FUNCTION__, __LINE__, name, diff, playTarget, playlistPosition + diff);
										mSyncAfterDiscontinuityInProgress = true;
										playTarget = playlistPosition + diff;
										discontinuity = false;
										programDateTime = NULL;
										ptr = next;
										continue;
									}
								}
							}
						}
						else
						{
							discontinuity = false;
						}
					}
					this->discontinuity = discontinuity || mSyncAfterDiscontinuityInProgress;
					mSyncAfterDiscontinuityInProgress = false;
					traceprintf("%s:%d [%s] Discontinuity - %d\n", __FUNCTION__, __LINE__, name, (int)this->discontinuity);
					rc = ptr;
					break;
				}
				else
				{
					discontinuity = false;
					programDateTime = NULL;
					// logprintf("Skipping fragment %s playlistPosition %f playTarget %f\n", ptr, playlistPosition, playTarget);
				}
			}
		}
		ptr = next;
	}
#ifdef TRACE
	logprintf("GetNextFragmentUriFromPlaylist :  pos %f returning %s\n", playlistPosition, rc);
	logprintf("seqNo=%lld\n", nextMediaSequenceNumber - 1);
#endif
	return rc;
}


/***************************************************************************
* @fn FindMediaForSequenceNumber
* @brief Get fragment tag based on media sequence number 
*		 
* @return String fragment tag line pointer 
***************************************************************************/
char *TrackState::FindMediaForSequenceNumber()
{
	char *ptr = playlist.ptr;
	long long mediaSequenceNumber = nextMediaSequenceNumber - 1;
	char *key = NULL;

	long long seq = 0;
	while (ptr)
	{
		char *next = mystrpbrk(ptr);
		if (*ptr)
		{
			if (startswith(&ptr, "#EXTINF:"))
			{
				fragmentDurationSeconds = atof(ptr);
			}
			else if (startswith(&ptr, "#EXT-X-MEDIA-SEQUENCE:"))
			{
				seq = atoll(ptr);
			}
			else if (startswith(&ptr, "#EXT-X-KEY:"))
			{
				key = ptr;
			}
			else if (ptr[0] != '#')
			{ // URI
				if (seq >= mediaSequenceNumber)
				{
					if ((mDrmKeyTagCount >1) && key)
					{
						ParseAttrList(key, ParseKeyAttributeCallback, this);
					}
					if (seq != mediaSequenceNumber)
					{
						logprintf("seq gap %lld!=%lld\n", seq, mediaSequenceNumber);
						nextMediaSequenceNumber = seq + 1;
					}
					return ptr;
				}
				seq++;
			}
		}
		ptr = next;
	}
	return NULL;
}
/***************************************************************************
* @fn FetchFragmentHelper
* @brief Helper function to download fragment 
*		 
* @param[out] http_error        Http error string 	
* @param[out] decryption_error  Decryption error
*
* @return bool true on success else false 
***************************************************************************/
bool TrackState::FetchFragmentHelper(long &http_error, bool &decryption_error)
{
#ifdef TRACE
		logprintf("FetchFragmentHelper Enter: pos %f start %f frag-duration %f fragmentURI %s\n",
				playlistPosition, playTarget, fragmentDurationSeconds, fragmentURI );
#endif
		assert (fragmentURI);
		if (context->trickplayMode && ABRManager::INVALID_PROFILE != context->GetIframeTrack())
		{
			fragmentURI = GetFragmentUriFromIndex();
			double delta = context->rate / context->mTrickPlayFPS;
			if (context->rate < 0)
			{ // rewind
				if (!fragmentURI || (playTarget == 0))
				{
					logprintf("aamp rew to beginning\n");
					eosReached = true;
				}
				else if (playTarget > -delta)
				{
					playTarget += delta;
				}
				else
				{
					playTarget = 0;
				}
			}
			else
			{// fast-forward
				if (!fragmentURI)
				{
					logprintf("aamp ffw to end\n");
					eosReached = true;
				}
				playTarget += delta;
			}
			//logprintf("Updated playTarget to %f\n", playTarget);
		}
		else
		{// normal speed
			fragmentURI = GetNextFragmentUriFromPlaylist();
			if (fragmentURI != NULL)
			{
				playTarget = playlistPosition + fragmentDurationSeconds;
				if (aamp->IsLive())
				{
					context->CheckForPlaybackStall(true);
				}
			}
			else
			{
				if ((ePLAYLISTTYPE_VOD == context->playlistType || context->hasEndListTag) && (playlistPosition != -1))
				{
					logprintf("aamp play to end. playTarget %f fragmentURI %p hasEndListTag %d\n", playTarget, fragmentURI, context->hasEndListTag);
					eosReached = true;
				}
				else if (aamp->IsLive() && type == eTRACK_VIDEO)
				{
					context->CheckForPlaybackStall(false);
				}
			}
		}

		if (fragmentURI)
		{
			char fragmentUrl[MAX_URI_LENGTH];
			CachedFragment* cachedFragment = GetFetchBuffer(true);
			aamp_ResolveURL(fragmentUrl, effectiveUrl, fragmentURI);
			traceprintf("Got next fragment url %s fragmentEncrypted %d discontinuity %d\n", fragmentUrl, fragmentEncrypted, (int)discontinuity);

			aamp->profiler.ProfileBegin(mediaTrackBucketTypes[type]);
			const char *range;
			char rangeStr[128];
			if (byteRangeLength)
			{
				int next = byteRangeOffset + byteRangeLength;
				sprintf(rangeStr, "%d-%d", byteRangeOffset, next - 1);
				logprintf("FetchFragmentHelper rangeStr %s \n", rangeStr);

				range = rangeStr;
			}
			else
			{
				range = NULL;
			}
#ifdef TRACE
			logprintf("FetchFragmentHelper: fetching %s\n", fragmentUrl);
#endif
			// patch for http://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8
			// if fragment URI uses relative path, we don't want to replace effective URI
			char tempEffectiveUrl[MAX_URI_LENGTH];
			traceprintf("%s:%d Calling Getfile . buffer %p avail %d\n", __FUNCTION__, __LINE__, &cachedFragment->fragment, (int)cachedFragment->fragment.avail);

			bool fetched = aamp->GetFile(fragmentUrl, &cachedFragment->fragment, tempEffectiveUrl, &http_error, range, type, false, (MediaType)(type));
			if (!fetched)
			{
				//cleanup is done in aamp_GetFile itself

				aamp->profiler.ProfileError(mediaTrackBucketTypes[type]);
				segDLFailCount += 1;
				if (AAMP_IS_LOG_WORTHY_ERROR(http_error))
				{
					logprintf("FetchFragmentHelper aamp_GetFile failed\n");
				}
				//Adding logic to report error if fragment downloads are failing continuously
				if(MAX_SEG_DOWNLOAD_FAIL_COUNT <= segDLFailCount && aamp->DownloadsAreEnabled())
				{
					logprintf("Not able to download fragments; reached failure threshold sending tune failed event\n");
					aamp->SendDownloadErrorEvent(AAMP_TUNE_FRAGMENT_DOWNLOAD_FAILURE, http_error);
				}
				aamp_Free(&cachedFragment->fragment.ptr);
				return false;
			}

			if((eTRACK_VIDEO == type)  && (aamp->IsTSBSupported()))
                        {
                                char *bwStr;
                                bwStr           =       strstr(tempEffectiveUrl,FOG_FRAG_BW_IDENTIFIER);
                                if(bwStr)
                                {
                                        bwStr           +=      FOG_FRAG_BW_IDENTIFIER_LEN;
                                        //  bwStr           =       strtok(bwStr,FOG_FRAG_BW_DELIMITER); this is not required as atol works with - terminated numbers.
                                        if(bwStr)
                                        {
                                                context->SetTsbBandwidth(atol(bwStr));
                                        }
                                }
                        }

			aamp->profiler.ProfileEnd(mediaTrackBucketTypes[type]);
			segDLFailCount = 0;

			if (cachedFragment->fragment.len && fragmentEncrypted)
			{
				{	
					traceprintf("%s:%d [%s] uri %s - calling  DrmDecrypt()\n", __FUNCTION__, __LINE__, name, fragmentURI);
					DrmReturn drmReturn = DrmDecrypt(cachedFragment, mediaTrackDecryptBucketTypes[type]);

					if(eDRM_SUCCESS != drmReturn)
					{
						logprintf("FetchFragmentHelper : drm_Decrypt failed. fragmentURI %s - RetryCount %d\n", fragmentURI, segDrmDecryptFailCount);
						if (aamp->DownloadsAreEnabled())
						{
							if (eDRM_KEY_ACQUSITION_TIMEOUT == drmReturn)
							{
								decryption_error = true;
								logprintf("FetchFragmentHelper : drm_Decrypt failed due to license acquisition timeout\n");
								aamp->SendErrorEvent(AAMP_TUNE_LICENCE_TIMEOUT, NULL, false);
							}
							else
							{
								/* Added to send tune error when fragments decryption failed */
								segDrmDecryptFailCount +=1;

								if(MAX_SEG_DRM_DECRYPT_FAIL_COUNT <= segDrmDecryptFailCount)
								{
									decryption_error = true;
									logprintf("FetchFragmentHelper : drm_Decrypt failed for fragments, reached failure threshold sending failure event\n");
									aamp->SendErrorEvent(AAMP_TUNE_DRM_DECRYPT_FAILED);
								}
							}
						}
						aamp_Free(&cachedFragment->fragment.ptr);
						return false;
					}
#ifdef TRACE
					else
					{
						logprintf("aamp: hls - eMETHOD_AES_128 not set for %s\n", fragmentURI);
					}
#endif
					segDrmDecryptFailCount = 0; /* Resetting the retry count in the case of decryption success */
				}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
				context->HarvestFile(fragmentUrl, &cachedFragment->fragment, true);
#endif
				if (!context->firstFragmentDecrypted)
				{
					aamp->NotifyFirstFragmentDecrypted();
					context->firstFragmentDecrypted = true;
				}
			}
			else if(!cachedFragment->fragment.len)
			{
				logprintf("fragment. len zero for %s\n", fragmentURI);
			}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
			else
			{
				context->HarvestFile(fragmentUrl, &cachedFragment->fragment, true);
			}
#endif
		}
		else
		{
			if (fragmentURI)
			{
				// null fragment URI technically not an error - live manifest may simply not have updated yet
				// if real problem exists, underflow will eventually be detected/reported
				logprintf("FetchFragmentHelper : fragmentURI %s playTarget(%f), playlistPosition(%f)\n", fragmentURI, playTarget, playlistPosition);
			}
			return false;
		}
		return true;
}
/***************************************************************************
* @fn FetchFragment
* @brief Function to fetch fragment  
*		 
* @return void
***************************************************************************/
void TrackState::FetchFragment()
{
	int timeoutMs = -1;
	long http_error = 0;
	bool decryption_error = false;
	if (aamp->IsLive())
	{
		timeoutMs = context->maxIntervalBtwPlaylistUpdateMs - (int) (aamp_GetCurrentTimeMS() - lastPlaylistDownloadTimeMS);
		if(timeoutMs < 0)
		{
			timeoutMs = 0;
		}
	}
	if (!WaitForFreeFragmentAvailable(timeoutMs))
	{
		return;
	}
	AAMPLOG_INFO("%s:%d: %s\n", __FUNCTION__, __LINE__, name);
	//DELIA-33346 -- always set the rampdown flag to false .
	context->mCheckForRampdown = false;
	if (false == FetchFragmentHelper(http_error, decryption_error))
	{
		if (fragmentURI)
		{
			context->lastSelectedProfileIndex = context->currentProfileIndex;
			// DELIA-32287 - Profile RampDown check and rampdown is needed only for Video . If audio fragment download fails
			// should continue with next fragment,no retry needed .
			if ((eTRACK_VIDEO == type) && context->CheckForRampDownProfile(http_error))
			{
				if (context->rate == AAMP_NORMAL_PLAY_RATE)
				{
					playTarget -= fragmentDurationSeconds;
				}
				else
				{
					playTarget -= context->rate / context->mTrickPlayFPS;
				}
				logprintf("%s:%d: Error while fetching fragment:%s, failedCount:%d. decrementing profile\n", __FUNCTION__, __LINE__, name, segDLFailCount);
				//DELIA-33346 -- if rampdown attempted , then set the flag so that abr is not attempted . 
				context->mCheckForRampdown = true;
			}
			else if (decryption_error)
			{
				logprintf("%s:%d: Error while decrypting fragments. failedCount:%d\n", __FUNCTION__, __LINE__, segDLFailCount);
			}
			else if (AAMP_IS_LOG_WORTHY_ERROR(http_error))
			{
				logprintf("%s:%d: Error on fetching %s fragment. failedCount:%d\n", __FUNCTION__, __LINE__, name, segDLFailCount);
			}
		}
		else
		{
			// technically not an error - live manifest may simply not have updated yet
			// if real problem exists, underflow will eventually be detected/reported
			AAMPLOG_TRACE("%s:%d: NULL fragmentURI for %s track \n", __FUNCTION__, __LINE__, name);
			fflush(stdout); // needed?
			//abort();
		}
		return;
	}
	CachedFragment* cachedFragment = GetFetchBuffer(false);
	if (cachedFragment->fragment.ptr)
	{
		double duration = fragmentDurationSeconds;
		double position = playTarget - playTargetOffset;
		if (context->rate == AAMP_NORMAL_PLAY_RATE)
		{
			position -= fragmentDurationSeconds;
			cachedFragment->discontinuity = discontinuity;
		}
		else
		{
			position -= context->rate / context->mTrickPlayFPS;
			cachedFragment->discontinuity = true;
			traceprintf("%s:%d: rate %f position %f\n",__FUNCTION__, __LINE__, context->rate, position);
		}

		if (context->trickplayMode && (0 != context->rate))
		{
			duration = (int)(duration*context->rate / context->mTrickPlayFPS);
		}
		cachedFragment->duration = duration;
		cachedFragment->position = position;
	}
	else
	{
		logprintf("%s:%d: %s cachedFragment->fragment.ptr is NULL\n", __FUNCTION__, __LINE__, name);
	}
#ifdef AAMP_DEBUG_INJECT
	if ((1 << type) & AAMP_DEBUG_INJECT)
	{
		strcpy(cachedFragment->uri, fragmentURI);
	}
#endif
	UpdateTSAfterFetch();
}
/***************************************************************************
* @fn InjectFragmentInternal
* @brief Injected decrypted fragment for playback 
*		 
* @param[in]  cachedFragment     CachedFragment structure 	
* @param[out] fragmentDiscarded  Bool to indicate fragment successfully injected
*
* @return void
***************************************************************************/
void TrackState::InjectFragmentInternal(CachedFragment* cachedFragment, bool &fragmentDiscarded)
{
#ifndef SUPRESS_DECODE
#ifndef FOG_HAMMER_TEST // support aamp stress-tests of fog without video decoding/presentation
			if (playContext)
			{
				double position = 0;
				if(!context->mStartTimestampZero)
				{
					position = cachedFragment->position;
				}
				fragmentDiscarded = !playContext->sendSegment(cachedFragment->fragment.ptr, cachedFragment->fragment.len,
						position, cachedFragment->duration, cachedFragment->discontinuity, ptsError);
			}
			else
			{
				fragmentDiscarded = false;
				aamp->SendStream((MediaType)type, cachedFragment->fragment.ptr, cachedFragment->fragment.len,
				        cachedFragment->position, cachedFragment->position, cachedFragment->duration);
			}
#endif
#endif
} // InjectFragmentInternal
/***************************************************************************
* @fn GetCompletionTimeForFragment
* @brief Function to get end time of fragment
*		 
* @param[in] trackState          TrackState structure	
* @param[in] mediaSequenceNumber Sequence number 
*
* @return double end time 
***************************************************************************/
static double GetCompletionTimeForFragment(const TrackState *trackState, long long mediaSequenceNumber)
{
	double rc = 0.0;
	int indexCount = trackState->indexCount;
	if (indexCount>0)
	{
		int idx = (int)(mediaSequenceNumber - trackState->indexFirstMediaSequenceNumber);
		if (idx >= 0)
		{
			if (idx >= indexCount)
			{
				idx = indexCount - 1;
			}
			const IndexNode *node = &((IndexNode *)trackState->index.ptr)[idx];
			rc = node->completionTimeSecondsFromStart;
		}
		else
		{
			logprintf("aamp warn - bad index!\n");
		}
	}
	return rc;
}

#ifdef TRACE
/***************************************************************************
* @fn DumpIndex
* @brief Function to log stored media information 
*		 
* @param[in] trackState TrackState structure
* @return void
***************************************************************************/
static void DumpIndex(TrackState *trackState)
{
	logprintf("index (%d fragments)\n", trackState->indexCount);
	long long mediaSequenceNumber = trackState->indexFirstMediaSequenceNumber;
	for (int idx = 0; idx < trackState->indexCount; idx++)
	{
		const IndexNode *node = &((IndexNode *)trackState->index.ptr)[idx];
		logprintf("%lld: %f %d\n", mediaSequenceNumber, node->completionTimeSecondsFromStart, node->drmMetadataIdx);
		mediaSequenceNumber++;
	}
}
#endif
/***************************************************************************
* @fn FlushIndex
* @brief Function to flush all stored data before refresh and stop
*		 
* @return void
***************************************************************************/
void TrackState::FlushIndex()
{
	aamp_Free(&index.ptr);
	indexFirstMediaSequenceNumber = 0;
	indexCount = 0;
	index.len = 0;
	index.avail = 0;
	currentIdx = -1;
	mDrmKeyTagCount = 0;
	mDiscontinuityIndexCount = 0;
	aamp_Free(&mDiscontinuityIndex.ptr);
	memset(&mDiscontinuityIndex, 0, sizeof(mDiscontinuityIndex));
	if (mDrmMetaDataIndexCount)
	{
		traceprintf("TrackState::%s:%d [%s]mDrmMetaDataIndexCount %d\n", __FUNCTION__, __LINE__, name,
		        mDrmMetaDataIndexCount);
		DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*) mDrmMetaDataIndex.ptr;
		assert(NULL != drmMetadataNode);
		for (int i = 0; i < mDrmMetaDataIndexCount; i++)
		{
			traceprintf("TrackState::%s:%d drmMetadataNode[%d].metaData.metadataPtr %p\n", __FUNCTION__, __LINE__, i,
			        drmMetadataNode[i].metaData.metadataPtr);

			if ((NULL == drmMetadataNode[i].metaData.metadataPtr || NULL == drmMetadataNode[i].sha1Hash) && mDrmMetaDataIndexCount)
			{
				logprintf ("TrackState::%s:%d **** metadataPtr/sha1Hash is NULL, give attention and analyze it... mDrmMetaDataIndexCount[%d]\n", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
			}

			if (drmMetadataNode[i].metaData.metadataPtr)
			{
				free(drmMetadataNode[i].metaData.metadataPtr);
				drmMetadataNode[i].metaData.metadataPtr = NULL;
			}

			if (drmMetadataNode[i].sha1Hash)
			{
				free(drmMetadataNode[i].sha1Hash);
				drmMetadataNode[i].sha1Hash = NULL;
			}
		}
		aamp_Free(&mDrmMetaDataIndex.ptr);
		memset(&mDrmMetaDataIndex, 0, sizeof(mDrmMetaDataIndex));
		mDrmMetaDataIndexCount = 0;
		mDrmMetaDataIndexPosition = 0;
	}
	mInitFragmentInfo = NULL;
}

/***************************************************************************
* @fn ProcessDrmMetadata
* @brief Process Drm Metadata after indexing
***************************************************************************/
void TrackState::ProcessDrmMetadata(bool acquireCurrentLicenseOnly)
{
	traceprintf("%s:%d: mDrmMetaDataIndexCount %d \n", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
	DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
	bool foundCurrentMetaDataIndex = false;
	pthread_mutex_lock(&gDrmMutex);
	/* Acquire license if license rotation not enabled (mCMSha1Hash NULL) or
	 * matches current fragment's meta-data or
	 * acquireCurrentLicenseOnly flag not set and license is not deferred*/
	for (int i = 0; i < mDrmMetaDataIndexCount; i++)
	{
		if (mCMSha1Hash)
		{
			if (!foundCurrentMetaDataIndex && (0 == memcmp(mCMSha1Hash, drmMetadataNode[i].sha1Hash, DRM_SHA1_HASH_LEN)))
			{
				mDrmMetaDataIndexPosition = i;
				foundCurrentMetaDataIndex = true;
			}
			else
			{
				if ( acquireCurrentLicenseOnly )
				{
					printf("%s:%d Not acquiring license for index %d mDrmMetaDataIndexCount %d since it is not the current metadata. sha1Hash - ", __FUNCTION__, __LINE__, i, mDrmMetaDataIndexCount);
					AveDrmManager::PrintSha1Hash(drmMetadataNode[i].sha1Hash);
					continue;
				}
				if ( gDeferredDrmLicTagUnderProcessing && gDeferredDrmLicRequestPending && (0 == memcmp(gDeferredDrmMetaDataSha1Hash, drmMetadataNode[i].sha1Hash, DRM_SHA1_HASH_LEN)))
				{
					logprintf("%s:%d: Not setting  metadata for index %d as deferred\n", __FUNCTION__, __LINE__, i);
					continue;
				}
			}
		}
		traceprintf("%s:%d: Setting  metadata for index %d\n", __FUNCTION__, __LINE__, i);
		AveDrmManager::SetMetadata(context->aamp, &drmMetadataNode[i],(int)type);
	}
	mDrmLicenseRequestPending = (mCMSha1Hash && acquireCurrentLicenseOnly && (mDrmMetaDataIndexCount >1));
	if(mCMSha1Hash && !foundCurrentMetaDataIndex)
	{
		printf("%s:%d ERROR Could not find matching metadata for hash - ", __FUNCTION__, __LINE__);
		AveDrmManager::PrintSha1Hash(mCMSha1Hash);
		printf("%d Metadata available\n", mDrmMetaDataIndexCount);
		for (int i = 0; i < mDrmMetaDataIndexCount; i++)
		{
			printf("sha1Hash of drmMetadataNode[%d]", i);
			AveDrmManager::PrintSha1Hash(drmMetadataNode[i].sha1Hash);
		}
		printf("\n\nTrack [%s] playlist length %d\n", name, (int)playlist.len);
		for (int i = 0; i < playlist.len; i++)
		{
			printf("%c", playlist.ptr[i]);
		}
		printf("\n\nTrack [%s] playlist end\n", name);
		aamp->SendErrorEvent(AAMP_TUNE_INVALID_MANIFEST_FAILURE, NULL, true);
	}
	traceprintf("%s:%d: mDrmLicenseRequestPending %d\n", __FUNCTION__, __LINE__, (int) mDrmLicenseRequestPending);
	pthread_mutex_unlock(&gDrmMutex);
}

/***************************************************************************
* @brief Start deferred DRM license acquisition
***************************************************************************/
void TrackState::StartDeferredDrmLicenseAcquisition()
{
	logprintf("%s:%d: mDrmMetaDataIndexCount %d Start deferred license request\n", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
	DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
	for (int i = (mDrmMetaDataIndexCount-1); i >= 0; i--)
	{
		if(drmMetadataNode[i].sha1Hash)
		{
			if (0 == memcmp(gDeferredDrmMetaDataSha1Hash, drmMetadataNode[i].sha1Hash, DRM_SHA1_HASH_LEN))
			{
				logprintf("%s:%d: Found matching drmMetadataNode index %d\n", __FUNCTION__, __LINE__, i);
				AveDrmManager::SetMetadata(context->aamp, &drmMetadataNode[i],(int)type);
				gDeferredDrmLicRequestPending = false;
				break;
			}
		}
	}
	if(gDeferredDrmLicRequestPending)
	{
		logprintf("%s:%d: WARNING - Could not start deferred license request - no matching sha1Hash\n", __FUNCTION__, __LINE__);
	}
}


/***************************************************************************
* @fn IndexPlaylist
* @brief Function to parse playlist 
*		 
* @return double  Total duration from playlist
***************************************************************************/
void TrackState::IndexPlaylist()
{
	double totalDuration = 0.0;
	char * playlistBackup = NULL;
	traceprintf("%s:%d Enter \n", __FUNCTION__, __LINE__);
	pthread_mutex_lock(&mPlaylistMutex);

	FlushIndex();
	mIndexingInProgress = true;
	if (playlist.ptr )
	{
		char *ptr;

		//ptr = strstr(playlist.ptr, "#EXTM3U");

		if(memcmp(playlist.ptr,"#EXTM3U",7)!=0)
		{
		    int tempDataLen = (MANIFEST_TEMP_DATA_LENGTH - 1);
		    char temp[MANIFEST_TEMP_DATA_LENGTH];
		    strncpy(temp, playlist.ptr, tempDataLen);
		    temp[tempDataLen] = 0x00;
		    logprintf("ERROR: Invalid Playlist URL:%s \n", playlistUrl);
		    logprintf("ERROR: Invalid Playlist DATA:%s \n", temp);
		    aamp->SendErrorEvent(AAMP_TUNE_INVALID_MANIFEST_FAILURE);
		    mDuration = totalDuration;
		    pthread_cond_signal(&mPlaylistIndexed);
		    pthread_mutex_unlock(&mPlaylistMutex);
		    return;
		}

		// TODO: may be more efficient to do the following scans as we walk the file

		ptr = strstr(playlist.ptr, "#EXT-X-MEDIA-SEQUENCE:");
		if (ptr)
		{
			ptr += 22;
			indexFirstMediaSequenceNumber = atoll(ptr);
		}
		else
		{ // for Sling content
			AAMPLOG_INFO("warning: no EXT-X-MEDIA-SEQUENCE tag\n");
			ptr = playlist.ptr;
			indexFirstMediaSequenceNumber = 0;
		}

		ptr = strstr(playlist.ptr, "#EXT-X-TARGETDURATION:");
		if( ptr )
		{
			ptr += 22;
			targetDurationSeconds = atof(ptr);
			AAMPLOG_INFO("aamp: EXT-X-TARGETDURATION = %f\n", targetDurationSeconds);
		}

		ptr = strstr(playlist.ptr, "#EXT-X-MAP:");
		if( ptr )
		{
			mInitFragmentInfo = ptr + 11;
			logprintf("%s:%d: #EXT-X-MAP for fragmented mp4 stream %p\n", __FUNCTION__, __LINE__, mInitFragmentInfo);
		}

		DrmMetadataNode drmMetadataNode;
		playlistBackup = (char*) malloc(playlist.len);
		memcpy(playlistBackup, playlist.ptr, playlist.len);
		ptr = playlist.ptr;
		do
		{
			char *drmPtr = NULL;
			ptr = strstr(ptr, "#EXT-X-FAXS-CM:");
			if (ptr)
			{
				drmPtr = ptr+strlen("#EXT-X-FAXS-CM:");
				traceprintf("aamp: #EXT-X-FAXS-CM:\n");

				char * delim = strchr(ptr, CHAR_LF);
				if (delim)
				{
					if (delim > ptr && delim[-1] == CHAR_CR)
					{
						delim--;
					}
					*delim = '\0';
					ptr = delim + 1;
				}
				else
				{
					ptr = NULL;
				}
				unsigned char hash[SHA_DIGEST_LENGTH] = {0};
				drmMetadataNode.metaData.metadataPtr =  base64_Decode(drmPtr, &drmMetadataNode.metaData.metadataSize);
				SHA1(drmMetadataNode.metaData.metadataPtr, drmMetadataNode.metaData.metadataSize, hash);
				drmMetadataNode.sha1Hash = base16_Encode(hash, SHA_DIGEST_LENGTH);
#ifdef TRACE
				logprintf("%s:%d [%s] drmMetadataNode[%d].sha1Hash -- ", __FUNCTION__, __LINE__, name, mDrmMetaDataIndexCount);
				for (int i = 0; i < DRM_SHA1_HASH_LEN; i++)
				{
					printf("%c", drmMetadataNode.sha1Hash[i]);
				}
				printf("\n");
#endif

				aamp_AppendBytes(&mDrmMetaDataIndex, &drmMetadataNode, sizeof(drmMetadataNode));
				traceprintf("%s:%d mDrmMetaDataIndex.ptr %p\n", __FUNCTION__, __LINE__, mDrmMetaDataIndex.ptr);
				mDrmMetaDataIndexCount++;
			}
		}
		while (ptr);
		if (mDrmMetaDataIndexCount > 1)
		{
			traceprintf("%s:%d Indexed %d drm metadata\n", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
		}
		memcpy(playlist.ptr, playlistBackup, playlist.len);

		ptr = strstr(playlist.ptr, "#EXT-X-PLAYLIST-TYPE:");
		if (ptr)
		{
			ptr += 21;
			// EVENT or VOD (optional); VOD if playlist will never change
			if (startswith(&ptr, "VOD"))
			{
				logprintf("aamp: EXT-X-PLAYLIST-TYPE - VOD\n");
				context->playlistType = ePLAYLISTTYPE_VOD;
			}
			else if (startswith(&ptr, "EVENT"))
			{
				logprintf("aamp: EXT-X-PLAYLIST-TYPE = EVENT\n");
				context->playlistType = ePLAYLISTTYPE_EVENT;
			}
			else
			{
				aamp_Error("unknown PLAYLIST-TYPE");
			}
		}

		if (context->playlistType != ePLAYLISTTYPE_VOD)
		{
			ptr = strstr(playlist.ptr, "#EXT-X-ENDLIST");
			if (ptr)
			{
				if (context->playlistType == ePLAYLISTTYPE_UNDEFINED)
				{
					logprintf("aamp: Found EXT-X-ENDLIST without EXT-X-PLAYLIST-TYPE\n");
				}
				else
				{
					logprintf("aamp: Found EXT-X-ENDLIST with ePLAYLISTTYPE_EVENT\n");
				}
				//required to avoid live adjust kicking in
				logprintf("aamp: Changing playlist type to ePLAYLISTTYPE_VOD as ENDLIST tag present\n");
				context->playlistType = ePLAYLISTTYPE_VOD;
			}
		}

		aamp->SetIsLive(context->playlistType != ePLAYLISTTYPE_VOD);

		aamp->mEnableCache = (context->playlistType == ePLAYLISTTYPE_VOD);
		if(aamp->mEnableCache)
		{
			logprintf("%s:%d [%s] Insert playlist to cache\n", __FUNCTION__, __LINE__, name);
			aamp->InsertToPlaylistCache(playlistUrl, &playlist, effectiveUrl);
		}
	}

	// DELIA-33434
	// Update DRM Manager for stored indexes so that it can be removed after playlist update
	// Update is required only for multi key stream, where Sha1 is set ,for single key stream,
	// SetMetadata is not called across playlist update , hence Update is not needed
	if(mCMSha1Hash)
	{
		AveDrmManager::UpdateBeforeIndexList(name,(int)type);
	}

	{ // build new index
		IndexNode node;
		node.completionTimeSecondsFromStart = 0.0;
		node.pFragmentInfo = NULL;
		char *ptr = playlist.ptr;
		int drmMetadataIdx = -1;
		const char* programDateTimeIdxOfFragment = NULL;
		bool discontinuity = false;
		bool deferDrmTagPresent = false;
		while (ptr)
		{
			// If enableSubscribedTags is true, examine each '#EXT' tag.
			ptr = strstr(ptr, "#EXT");
			if(ptr)
			{
				if (strncmp(ptr + 4, "INF:", 4) == 0)
				{
					if (discontinuity)
					{
						logprintf("%s:%d #EXT-X-DISCONTINUITY in track[%d] indexCount %d periodPosition %f\n", __FUNCTION__, __LINE__, type, indexCount, totalDuration);
						DiscontinuityIndexNode discontinuityIndexNode;
						discontinuityIndexNode.fragmentIdx = indexCount;
						discontinuityIndexNode.position = totalDuration;
						discontinuityIndexNode.programDateTime = programDateTimeIdxOfFragment;
						aamp_AppendBytes(&mDiscontinuityIndex, &discontinuityIndexNode, sizeof(DiscontinuityIndexNode));
						mDiscontinuityIndexCount++;
						discontinuity = false;
					}
					programDateTimeIdxOfFragment = NULL;
					node.pFragmentInfo = ptr;
					indexCount++;
					ptr += 8; // skip #EXTINF:
					totalDuration += atof(ptr);
					node.completionTimeSecondsFromStart = totalDuration;
					node.drmMetadataIdx = drmMetadataIdx;
					aamp_AppendBytes(&index, &node, sizeof(node));
				}
				else if (strncmp(ptr + 4, "-X-DISCONTINUITY", 16) == 0)
				{
					if (0 != totalDuration)
					{
						discontinuity = true;
					}
				}
				else if (strncmp(ptr + 4, "-X-PROGRAM-DATE-TIME:", 21) == 0)
				{
					programDateTimeIdxOfFragment = ptr+4+21;
					traceprintf("Got EXT-X-PROGRAM-DATE-TIME: %.*s \n", 30, programDateTimeIdxOfFragment);
				}
				else if (strncmp(ptr + 4, "-X-KEY:", 7) == 0)
				{
					traceprintf("aamp: EXT-X-KEY\n");
					ptr += strlen("#EXT-X-KEY:");
					char* delim = strchr(ptr, CHAR_LF);
					if (delim)
					{
						*delim = '\0';
					}
					ParseAttrList(ptr, ParseKeyAttributeCallback, this);
					//int prtOffset = ptr-
					//TODO optimize this
					memcpy(playlist.ptr, playlistBackup, playlist.len);
					drmMetadataIdx = mDrmMetaDataIndexPosition;
					if(!fragmentEncrypted)
					{
						drmMetadataIdx = -1;
						traceprintf("%s:%d Not encrypted - fragmentEncrypted %d mCMSha1Hash %p\n", __FUNCTION__, __LINE__, fragmentEncrypted, mCMSha1Hash);
					}
					mDrmKeyTagCount++;
				}
				else if ( aamp->IsLive() && (AAMP_NORMAL_PLAY_RATE == context->rate)
					&& ((eTUNETYPE_NEW_NORMAL == context->mTuneType) || (eTUNETYPE_SEEKTOLIVE == context->mTuneType))
					&& (strncmp(ptr + 4, "-X-X1-LIN-CK:", 13) == 0))
				{
					deferDrmTagPresent = true;

					pthread_mutex_lock(&gDrmMutex);
					if (!gDeferredDrmLicTagUnderProcessing )
					{
						ptr += 17;
						char* delim = strchr(ptr, CHAR_LF);
						if (delim)
						{
							long time = strtol(ptr, NULL, 10);
							logprintf("%s:%d [%s] #EXT-X-X1-LIN-CK:%d #####\n", __FUNCTION__, __LINE__, name, time);
							if (time != 0 )
							{
								if (mDrmMetaDataIndexCount > 1)
								{
									if (!firstIndexDone)
									{
										logprintf("%s:%d #EXT-X-X1-LIN-CK on first index - not deferring license acquisition\n", __FUNCTION__, __LINE__);
										gDeferredDrmLicRequestPending = false;
									}
									else
									{
										logprintf("%s:%d: mDrmMetaDataIndexCount %d\n", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
										DrmMetadataNode* drmMetadataIdx = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
										int deferredIdx = AveDrmManager::GetNewMetadataIndex( drmMetadataIdx, mDrmMetaDataIndexCount);
										if ( deferredIdx != -1)
										{
											logprintf("%s:%d: deferredIdx %d\n", __FUNCTION__, __LINE__, deferredIdx);
											char * sha1Hash = drmMetadataIdx[deferredIdx].sha1Hash;
											assert(sha1Hash);
											printf("%s:%d defer acquisition of meta-data with hash - ", __FUNCTION__, __LINE__);
											AveDrmManager::PrintSha1Hash(sha1Hash);
											memcpy(gDeferredDrmMetaDataSha1Hash, sha1Hash, DRM_SHA1_HASH_LEN);
											gDeferredDrmTime = aamp_GetCurrentTimeMS() + GetDeferTimeMs(time);
											gDeferredDrmLicRequestPending = true;
										}
										else
										{
											logprintf("%s:%d: GetNewMetadataIndex failed\n", __FUNCTION__, __LINE__);
										}
									}
									gDeferredDrmLicTagUnderProcessing = true;
								}
								else
								{
									logprintf("%s:%d: ERROR mDrmMetaDataIndexCount %d\n", __FUNCTION__, __LINE__, mDrmMetaDataIndexCount);
								}
							}
							else
							{
								logprintf("%s:%d: #EXT-X-X1-LIN-CK invalid time\n", __FUNCTION__, __LINE__);
							}
						}
						else
						{
							logprintf("%s:%d: #EXT-X-X1-LIN-CK - parse error\n", __FUNCTION__, __LINE__);

						}
					}
					pthread_mutex_unlock(&gDrmMutex);
				}
				else if (gpGlobalConfig->enableSubscribedTags && (eTRACK_VIDEO == type))
				{
					for (int i = 0; i < aamp->subscribedTags.size(); i++)
					{
						int len = aamp->subscribedTags.at(i).length();
						const char* data = aamp->subscribedTags.at(i).data();
						if (strncmp(ptr + 4, data + 4, len - 4) == 0)
						{
							char *fin = strchr(ptr, CHAR_LF);
							if (fin)
							{
								if (fin > ptr && fin[-1] == CHAR_CR)
								{
									fin--;
								}
							}
							else
							{
								fin = strchr(ptr, '\0');
							}
							int nb = fin - ptr;
							// logprintf("[AAMP_JS] Found subscribedTag[%d]: @%f '%.*s'\n", i, totalDuration, nb, ptr);
							aamp->ReportTimedMetadata(totalDuration * 1000, data, ptr, nb);
							break;
						}
					}
				}
				ptr += 4; // skip #EXT
		    }
		}
		if(eTRACK_VIDEO == type)
		{
			aamp->UpdateDuration(totalDuration);
		}

		if (gDeferredDrmLicTagUnderProcessing && !deferDrmTagPresent)
		{
			logprintf("%s:%d - reset gDeferredDrmLicTagUnderProcessing\n", __FUNCTION__, __LINE__);
			gDeferredDrmLicTagUnderProcessing = false;
		}
	}

	if(playlistBackup)
	{
		free(playlistBackup);
	}
#ifdef TRACE
	DumpIndex(this);
#endif
	if ((firstIndexDone && (NULL != mCMSha1Hash)) || mForceProcessDrmMetadata)
	{
		ProcessDrmMetadata(false);
		mForceProcessDrmMetadata = false;
	}
	if (mDrmKeyTagCount > 0)
	{
		if (mDrmMetaDataIndexCount > 0)
		{
			aamp->setCurrentDrm(eDRM_Adobe_Access);
		}
		else
		{
			aamp->setCurrentDrm(eDRM_Vanilla_AES);
		}
	}
	firstIndexDone = true;
	mIndexingInProgress = false;
	traceprintf("%s:%d Exit indexCount %d mDrmMetaDataIndexCount %d\n", __FUNCTION__, __LINE__, indexCount, mDrmMetaDataIndexCount);
	mDuration = totalDuration;
	// DELIA-33434
	// Update is required only for multi key stream, where Sha1 is set ,for single key stream,
	// SetMetadata is not called across playlist update hence flush is not needed
	if(mCMSha1Hash)
	{
		AveDrmManager::FlushAfterIndexList(name,(int)type);
	}
	pthread_cond_signal(&mPlaylistIndexed);
	pthread_mutex_unlock(&mPlaylistMutex);
}

#ifdef AAMP_HARVEST_SUPPORT_ENABLED
/***************************************************************************
* @fn HarvestFile
* @brief Function to harvest stream contents locally for debugging 
*		 
* @param[in] url         url string	
* @param[in] buffer      Data content
* @param[in] isFragment  Flag indicating fragment or not
* @param[in] prefix      Prefix string to add to file name 
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::HarvestFile(const char * url, GrowableBuffer* buffer, bool isFragment, const char* prefix)
{
	if (aamp->HarvestFragments(isFragment))
	{
		logprintf("aamp: hls Harvest %s len %d\n", url, (int)buffer->len);
		char path[1024];
		sprintf(path, "/media/tsb/"); // SD card on xi3v2
		const char *src = url;
		for (;;)
		{
			const char *delim = strchr(src, '/');
			if (delim)
			{
				src = delim + 1;
			}
			else
			{
				break;
			}
		}
		if (prefix)
		{
			strcat(path, prefix);
		}
		strcat(path, src);
		FILE *f = fopen(path, "wb");
		if (f)
		{
			fwrite(buffer->ptr, 1, buffer->len, f);
			fclose(f);
			logprintf("aamp: hls -harvest written %s buffer.len %d\n", path, (int)buffer->len);
		}
		else
		{
			logprintf("aamp: hls -harvest fopen failed %s len %d\n", path, (int)buffer->len);
		}
	}
}
#endif
/***************************************************************************
* @fn ABRProfileChanged
* @brief Function to handle Profile change after ABR  
*		 
* @return void
***************************************************************************/

void TrackState::ABRProfileChanged()
{
	// If not live, reset play position since sequence number doesn't ensure the fragments
	// from multiple streams are in sync
	traceprintf("%s:%d playlistPosition %f\n", __FUNCTION__,__LINE__, playlistPosition);
	aamp_ResolveURL(playlistUrl, aamp->GetManifestUrl(), context->GetPlaylistURI(type));
	pthread_mutex_lock(&mutex);
	//playlistPosition reset will be done by RefreshPlaylist once playlist downloaded successfully
	//refreshPlaylist is used to reset the profile index if playlist download fails! Be careful with it.
	refreshPlaylist = true;
	mInjectInitFragment = true;
	/*For some VOD assets, different video profiles have different DRM meta-data.*/
	mForceProcessDrmMetadata = true;
	pthread_mutex_unlock(&mutex);
}
/***************************************************************************
* @fn RefreshPlaylist
* @brief Function to redownload playlist after refresh interval .
*		 
* @return void
***************************************************************************/
void TrackState::RefreshPlaylist(void)
{
	// use current, not next position to synchronize and detect culling
	// important, as 'next position' typically isn't present in both
	// the before and after playlist
	long long commonPlayPosition = nextMediaSequenceNumber - 1;
	double prevSecondsBeforePlayPoint = GetCompletionTimeForFragment(this, commonPlayPosition);
	GrowableBuffer tempBuff;
	long http_error = 0;

	// note: this used to be updated only upon succesful playlist download
	// this can lead to back-to-back playlist download retries
	lastPlaylistDownloadTimeMS = aamp_GetCurrentTimeMS();

#ifdef WIN32
	logprintf("\npre-refresh %fs before %lld\n", prevSecondsBeforePlayPoint, commonPlayPosition);
#endif

	if(playlist.ptr)
	{
		tempBuff.len = playlist.len;
		tempBuff.avail = playlist.avail;
		tempBuff.ptr = playlist.ptr;
		memset(&playlist, 0, sizeof(playlist));
	}
	else
	{
		memset(&tempBuff, 0, sizeof(tempBuff));
	}

	aamp->GetFile(playlistUrl, &playlist, effectiveUrl, &http_error, NULL, type, true, eMEDIATYPE_MANIFEST);

	if (playlist.len)
	{ // download successful
		//lastPlaylistDownloadTimeMS = aamp_GetCurrentTimeMS();
		if (context->mNetworkDownDetected)
		{
			context->mNetworkDownDetected = false;
		}
		aamp_Free(&tempBuff.ptr);
		aamp_AppendNulTerminator(&playlist); // hack: make safe for cstring operationsaamp_AppendNulTerminator(&this->mainManifest); // make safe for cstring operations
		if (gpGlobalConfig->logging.trace)
		{
			logprintf("***New Playlist:**************\n\n%s\n*************\n", playlist.ptr);
		}
		IndexPlaylist();
		if( mDuration > 0.0f )
		{
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
		    const char* prefix = (type == eTRACK_AUDIO)?"aud-":(context->trickplayMode)?"ifr-":"vid-";
		    context->HarvestFile(playlistUrl, &playlist, false, prefix);
#endif
		    if (ePLAYLISTTYPE_VOD != context->playlistType)
		    {
		        fragmentURI = FindMediaForSequenceNumber();
		    }
		    else
		    {
		        fragmentURI = playlist.ptr;
		        playlistPosition = -1;
		    }
		    manifestDLFailCount = 0;
		}
	}
	else
	{
		//Restore playlist in case of failure
		if (tempBuff.ptr)
		{
			playlist.ptr = tempBuff.ptr;
			playlist.len = tempBuff.len;
			playlist.avail = tempBuff.avail;
			//Refresh happened due to ABR switching, we need to reset the profileIndex
			//so that ABR can be attempted later
			if (refreshPlaylist)
			{
				context->currentProfileIndex = context->lastSelectedProfileIndex;
			}
		}

		if (aamp->DownloadsAreEnabled())
		{
			if (CURLE_OPERATION_TIMEDOUT == http_error || CURLE_COULDNT_CONNECT == http_error)
			{
				context->mNetworkDownDetected = true;
				logprintf("%s:%d Ignore curl timeout\n", __FUNCTION__, __LINE__);
				return;
			}
			manifestDLFailCount++;

			if(fragmentURI == NULL && (manifestDLFailCount > MAX_MANIFEST_DOWNLOAD_RETRY))//No more fragments to download
			{
				aamp->SendDownloadErrorEvent(AAMP_TUNE_MANIFEST_REQ_FAILED, http_error);
				return;
			}
		}
	}
	double newSecondsBeforePlayPoint = GetCompletionTimeForFragment(this, commonPlayPosition);
	double culled = prevSecondsBeforePlayPoint - newSecondsBeforePlayPoint;
	mCulledSeconds += culled;
	if (eTRACK_VIDEO == type)
	{
#ifdef WIN32
		logprintf("post-refresh %fs before %lld (%f)\n\n", newSecondsBeforePlayPoint, commonPlayPosition, culled);
#endif
		aamp->UpdateCullingState(culled); // report amount of content that was implicitly culled since last playlist download
	}
}
/***************************************************************************
* @fn GetPlaylistURI
* @brief Function to get playlist URI based on media selection 
*		 
* @param[in] trackType  Track type 
* @param[in] format     Stream output type
*
* @return string playlist URI 
***************************************************************************/
const char *StreamAbstractionAAMP_HLS::GetPlaylistURI(TrackType trackType, StreamOutputFormat* format)
{
	const char *playlistURI = NULL;
	switch (trackType)
	{
	case eTRACK_VIDEO:
		playlistURI = this->streamInfo[this->currentProfileIndex].uri;
		if (format)
		{
			*format = FORMAT_MPEGTS;
		}
		break;

	case eTRACK_AUDIO:
		HlsStreamInfo* streamInfo = &this->streamInfo[this->currentProfileIndex];
		const char* group = streamInfo->audio;
		if (group)
		{
			logprintf("GetPlaylistURI : AudioTrack: group %s, aamp->language %s\n", group, aamp->language);
			bool foundAudio = false;
			for(int langChecks = aamp->language[0] ? 2 : 1; langChecks > 0 && !foundAudio; --langChecks)
			{
				for (int i = 0; i < this->mediaCount; i++)
				{
	#ifdef TRACE
					logprintf("GetPlaylistURI : AudioTrack: this->mediaInfo[%d].group_id %s\n", i,
						this->mediaInfo[i].group_id);
	#endif
					if (this->mediaInfo[i].group_id && !strcmp(group, this->mediaInfo[i].group_id))
					{
//	#ifdef TRACE
						logprintf("GetPlaylistURI checking if preferred language '%s' matches media[%d] language '%s'\n", aamp->language, i, this->mediaInfo[i].language);
//	#endif
						if ( (aamp->language[0] && strncmp(aamp->language, this->mediaInfo[i].language, MAX_LANGUAGE_TAG_LENGTH)==0) || (langChecks == 1 && this->mediaInfo[i].isDefault) )
						{
							foundAudio = true;
							if(langChecks == 1)
							{
								//save what language we have selected, defaulting to english
								logprintf("%s updating aamp->language from %s to %s mediaInfo[i].language %s\n", __FUNCTION__, aamp->language, this->mediaInfo[i].language ? this->mediaInfo[i].language : "en", mediaInfo[i].language);
								aamp->UpdateAudioLanguageSelection((this->mediaInfo[i].language ? this->mediaInfo[i].language : "en"));
								logprintf("GetPlaylistURI : language not found. Instead, select default of %s\n", aamp->language);
							}
							playlistURI = this->mediaInfo[i].uri;
							logprintf("GetPlaylistURI language found uri %s\n", playlistURI);
							if (playlistURI)
							{
								logprintf("GetPlaylistURI : AudioTrack: playlistURI %s\n", playlistURI);
							}
							else
							{
								logprintf("GetPlaylistURI : AudioTrack: NULL playlistURI. this->mediaInfo[i].isDefault %d\n", this->mediaInfo[i].isDefault);
							}
							if (format)
							{
								*format = FORMAT_NONE;
								if (this->mediaInfo[i].uri && streamInfo->codecs)
								{
	#ifdef TRACE
									logprintf("GetPlaylistURI : AudioTrack: streamInfo->codec %s\n", streamInfo->codecs);
	#endif
									for (int j = 0; j < AAMP_AUDIO_FORMAT_MAP_LEN; j++)
									{
										if (strstr(streamInfo->codecs, audioFormatMap[j].codec))
										{
											*format = audioFormatMap[j].format;
											logprintf("GetPlaylistURI : AudioTrack: Audio format is %d [%s]\n",
												audioFormatMap[j].format, audioFormatMap[j].codec);
											break;
										}
									}
								}
							}
							break;
						}
					}
				}
			}
		}
		else if (!trickplayMode)
		{
			logprintf("%s updating aamp->language from %s to \"en\"\n", __FUNCTION__, aamp->language);
			//save that we are using english by default
			aamp->UpdateAudioLanguageSelection("en");
		}
		break;
	}
	return playlistURI;
}
/***************************************************************************
* @fn GetFormatFromFragmentExtension
* @brief Function to get media format based on fragment extension
*		 
* @param[in] trackState  TrackState structure pointer
*
* @return StreamOutputFormat stream format
***************************************************************************/
static StreamOutputFormat GetFormatFromFragmentExtension(TrackState *trackState)
{
	StreamOutputFormat format = FORMAT_INVALID;
	std::istringstream playlistStream(trackState->playlist.ptr);
	for (std::string line; std::getline(playlistStream, line); )
	{
		if(!line.empty() && (line.c_str()[0] != '#'))
		{
			while(isspace(line.back()))
			{
				line.pop_back();
				if (line.empty())
				{
				    break;
				}
			}
			if (line.empty())
			{
			    continue;
			}
			traceprintf("%s:%d line === %s ====\n", __FUNCTION__, __LINE__, line.c_str());
			size_t end = line.find("?");
			if (end != std::string::npos)
			{
				line = line.substr(0, end);
			}
			size_t extenstionStart = line.find_last_of('.');
			if (extenstionStart != std::string::npos)
			{
				std::string extension = line.substr(extenstionStart);
				traceprintf("%s:%d extension %s\n", __FUNCTION__, __LINE__, extension.c_str());
				if (0 == extension.compare(".ts"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_MPEGTS\n", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_MPEGTS;
				}
				else if (0 == extension.compare(".mp4"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_ISO_BMFF\n", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_ISO_BMFF;
				}
				else if (0 == extension.compare(".aac"))
				{
					logprintf("%s:%d fragment extension %s - FORMAT_AUDIO_ES_AAC\n", __FUNCTION__, __LINE__, extension.c_str());
					format = FORMAT_AUDIO_ES_AAC;
				}
				else
				{
					logprintf("%s:%d Not TS or MP4 extension, probably ES. fragment extension %s len %d\n", __FUNCTION__, __LINE__, extension.c_str(), strlen(extension.c_str()));
				}
			}
			else
			{
				logprintf("%s:%d Could not find extension from line %s\n", __FUNCTION__, __LINE__, line.c_str());
			}
			break;
		}
	}
	return format;
}

/***************************************************************************
* @fn SyncVODTracks
* @brief Function to synchronize time between audio & video for VOD stream
*		 
* @return eAAMPSTATUS_OK on success
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::SyncTracksForDiscontinuity()
{
	TrackState *audio = trackState[eMEDIATYPE_AUDIO];
	TrackState *video = trackState[eMEDIATYPE_VIDEO];
	AAMPStatusType retVal = eAAMPSTATUS_GENERIC_ERROR;
	if (audio->GetNumberOfPeriods() == video->GetNumberOfPeriods())
	{
		int periodIdx;
		double offsetFromPeriod;
		video->GetNextFragmentPeriodInfo( periodIdx, offsetFromPeriod);
		if(-1 != periodIdx)
		{
			logprintf("%s:%d video periodIdx %d offsetFromPeriod %f\n", __FUNCTION__, __LINE__, periodIdx, offsetFromPeriod);
			double audioPeriodStart = audio->GetPeriodStartPosition(periodIdx);
			if (0 != audioPeriodStart )
			{
				audio->playTarget = audioPeriodStart + offsetFromPeriod;
				retVal = eAAMPSTATUS_OK;
			}
			else
			{
				logprintf("%s:%d audioDiscontinuityOffset 0\n", __FUNCTION__, __LINE__);
			}
		}
	}
	else
	{
		logprintf("%s:%d WARNING audio's number of period %d video number of period %d\n", __FUNCTION__, __LINE__, audio->GetNumberOfPeriods(), video->GetNumberOfPeriods());
	}
	logprintf("%s Exit : audio track start %f, vid track start %f\n", __FUNCTION__, audio->playTarget, video->playTarget );
	return retVal;
}

/***************************************************************************
* @fn SyncTracks
* @brief Function to synchronize time between A/V for Live/Event assets
* @param useProgramDateTimeIfAvailable use program date time tag to sync if available
* @return eAAMPSTATUS_OK on success
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::SyncTracks(bool useProgramDateTimeIfAvailable)
{
	AAMPStatusType retval = eAAMPSTATUS_OK;
	bool startTimeAvailable = true;
	bool syncedUsingSeqNum = false;
	long long mediaSequenceNumber[AAMP_TRACK_COUNT];
	TrackState *audio = trackState[eMEDIATYPE_AUDIO];
	TrackState *video = trackState[eMEDIATYPE_VIDEO];
	double diffBetweenStartTimes = 0;
	for(int i = 0; i<AAMP_TRACK_COUNT; i++)
	{
		TrackState *ts = trackState[i];
		ts->fragmentURI = trackState[i]->GetNextFragmentUriFromPlaylist(true); //To parse track playlist
		/*Update playTarget to playlistPostion to correct the seek position to start of a fragment*/
		ts->playTarget = ts->playlistPosition;
		logprintf("syncTracks loop : track[%d] pos %f start %f frag-duration %f trackState->fragmentURI %s ts->nextMediaSequenceNumber %lld\n", i, ts->playlistPosition, ts->playTarget, ts->fragmentDurationSeconds, ts->fragmentURI, ts->nextMediaSequenceNumber);
		if (ts->startTimeForPlaylistSync.tv_sec == 0)
		{
			logprintf("startTime not available for track %d\n", i);
			startTimeAvailable = false;
		}
		mediaSequenceNumber[i] = ts->nextMediaSequenceNumber - 1;
	}

	if (startTimeAvailable)
	{
		diffBetweenStartTimes = trackState[eMEDIATYPE_AUDIO]->startTimeForPlaylistSync.tv_sec - trackState[eMEDIATYPE_VIDEO]->startTimeForPlaylistSync.tv_sec
			+ (float)(trackState[eMEDIATYPE_AUDIO]->startTimeForPlaylistSync.tv_usec - trackState[eMEDIATYPE_VIDEO]->startTimeForPlaylistSync.tv_usec) / 1000000;
		if (!useProgramDateTimeIfAvailable)
		{
			if (video->targetDurationSeconds != audio->targetDurationSeconds)
			{
				logprintf("%s:%d WARNING seqno based track synchronization when video->targetDurationSeconds[%f] != audio->targetDurationSeconds[%f]\n",
				        __FUNCTION__, __LINE__, video->targetDurationSeconds, audio->targetDurationSeconds);
			}
			else
			{
				double diffBasedOnSeqNumber = (mediaSequenceNumber[eMEDIATYPE_AUDIO]
				        - mediaSequenceNumber[eMEDIATYPE_VIDEO]) * video->fragmentDurationSeconds;
				if (fabs(diffBasedOnSeqNumber - diffBetweenStartTimes) > video->fragmentDurationSeconds)
				{
					logprintf("%s:%d WARNING - inconsistency between startTime and seqno  startTime diff %f diffBasedOnSeqNumber %f\n",
					        __FUNCTION__, __LINE__, diffBetweenStartTimes, diffBasedOnSeqNumber);
				}
			}
		}

		if((diffBetweenStartTimes < -10 || diffBetweenStartTimes > 10))
		{
			logprintf("syncTracks diff debug : Audio start time sec : %ld  Video start time sec : %ld \n",
			trackState[eMEDIATYPE_AUDIO]->startTimeForPlaylistSync.tv_sec,
			trackState[eMEDIATYPE_VIDEO]->startTimeForPlaylistSync.tv_sec);
		}
	}
	if (!startTimeAvailable || !useProgramDateTimeIfAvailable)
	{
		MediaType mediaType;
#ifdef TRACE
		logprintf("%s:%d sync using sequence number. A %lld V %lld a-f-uri %s v-f-uri %s\n", __FUNCTION__,
				__LINE__, mediaSequenceNumber[eMEDIATYPE_AUDIO], mediaSequenceNumber[eMEDIATYPE_VIDEO],
				trackState[eMEDIATYPE_AUDIO]->fragmentURI, trackState[eMEDIATYPE_VIDEO]->fragmentURI);
#endif
		TrackState *laggingTS = NULL;
		long long diff = 0;
		if (mediaSequenceNumber[eMEDIATYPE_AUDIO] > mediaSequenceNumber[eMEDIATYPE_VIDEO])
		{
			laggingTS = video;
			diff = mediaSequenceNumber[eMEDIATYPE_AUDIO] - mediaSequenceNumber[eMEDIATYPE_VIDEO];
			mediaType = eMEDIATYPE_VIDEO;
			logprintf("%s:%d video track lag in seqno. diff %lld\n", __FUNCTION__, __LINE__, diff);
		}
		else if (mediaSequenceNumber[eMEDIATYPE_VIDEO] > mediaSequenceNumber[eMEDIATYPE_AUDIO])
		{
			laggingTS = audio;
			diff = mediaSequenceNumber[eMEDIATYPE_VIDEO] - mediaSequenceNumber[eMEDIATYPE_AUDIO];
			mediaType = eMEDIATYPE_AUDIO;
			logprintf("%s:%d audio track lag in seqno. diff %lld\n", __FUNCTION__, __LINE__, diff);
		}
		if (laggingTS)
		{
			if (startTimeAvailable && (diff > MAX_SEQ_NUMBER_DIFF_FOR_SEQ_NUM_BASED_SYNC))
			{
				logprintf("%s:%d - falling back to synchronization based on start time as diff = %lld\n", __FUNCTION__, __LINE__, diff);
			}
			else if ((diff <= MAX_SEQ_NUMBER_LAG_COUNT) && (diff > 0))
			{
				logprintf("%s:%d sync using sequence number. diff [%lld] A [%lld] V [%lld] a-f-uri [%s] v-f-uri [%s]\n", __FUNCTION__,
						__LINE__, diff, mediaSequenceNumber[eMEDIATYPE_AUDIO], mediaSequenceNumber[eMEDIATYPE_VIDEO],
						trackState[eMEDIATYPE_AUDIO]->fragmentURI, trackState[eMEDIATYPE_VIDEO]->fragmentURI);
				while (diff > 0)
				{
					laggingTS->playTarget += laggingTS->fragmentDurationSeconds;
					laggingTS->playTargetOffset += laggingTS->fragmentDurationSeconds;
					if (laggingTS->fragmentURI)
					{
						laggingTS->fragmentURI = laggingTS->GetNextFragmentUriFromPlaylist(true);
					}
					else
					{
						logprintf("%s:%d laggingTS->fragmentURI NULL, seek might be out of window\n", __FUNCTION__, __LINE__);
					}
					diff--;
				}
				syncedUsingSeqNum = true;
			}
			else
			{
				logprintf("%s:%d Lag in '%s' seq no, diff[%lld] > maxValue[%d]\n",
						__FUNCTION__, __LINE__, ((eMEDIATYPE_VIDEO == mediaType) ? "video" : "audio"), diff, MAX_SEQ_NUMBER_LAG_COUNT);
			}
		}
		else
		{
			logprintf("%s:%d No lag in seq no b/w AV\n", __FUNCTION__, __LINE__);
			syncedUsingSeqNum = true;
		}
	}

	if (!syncedUsingSeqNum)
	{
		if (startTimeAvailable)
		{
			if (diffBetweenStartTimes > 0)
			{
				TrackState *ts = trackState[eMEDIATYPE_VIDEO];
				if (diffBetweenStartTimes > (ts->fragmentDurationSeconds / 2))
				{
					if (video->mDuration > (ts->playTarget + diffBetweenStartTimes))
					{
						logprintf("%s:%d Audio track in front, catchup videotrack\n", __FUNCTION__, __LINE__);
						ts->playTarget += diffBetweenStartTimes;
						ts->playTargetOffset = diffBetweenStartTimes;
					}
					else
					{
						logprintf("%s:%d invalid diff %f ts->playTarget %f trackDuration %f\n", __FUNCTION__, __LINE__,
						        diffBetweenStartTimes, ts->playTarget, video->mDuration);
						retval = eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR;
					}
				}
				else
				{
					logprintf("syncTracks : Skip playTarget updation diff %f, vid track start %f fragmentDurationSeconds %f\n",
					        diffBetweenStartTimes, ts->playTarget, ts->fragmentDurationSeconds);
				}
			}
			else if (diffBetweenStartTimes < 0)
			{
				TrackState *ts = trackState[eMEDIATYPE_AUDIO];
				if (fabs(diffBetweenStartTimes) > (ts->fragmentDurationSeconds / 2))
				{
					if (audio->mDuration > (ts->playTarget - diffBetweenStartTimes))
					{
						logprintf("%s:%d Video track in front, catchup audio track\n", __FUNCTION__, __LINE__);
						ts->playTarget -= diffBetweenStartTimes;
						ts->playTargetOffset = -diffBetweenStartTimes;
					}
					else
					{
						logprintf("%s:%d invalid diff %f ts->playTarget %f trackDuration %f\n", __FUNCTION__, __LINE__,
						        diffBetweenStartTimes, ts->playTarget, audio->mDuration);
						retval = eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR;
					}
				}
				else
				{
					logprintf(
					        "syncTracks : Skip playTarget updation diff %f, aud track start %f fragmentDurationSeconds %f\n",
					        fabs(diffBetweenStartTimes), ts->playTarget, ts->fragmentDurationSeconds);
				}
			}
		}
		else
		{
			logprintf("%s:%d Could not sync using seq num and start time not available., cannot play this content.!!\n",
			        __FUNCTION__, __LINE__);
			retval = eAAMPSTATUS_TRACKS_SYNCHRONISATION_ERROR;
		}
	}
	logprintf("syncTracks Exit : audio track start %f, vid track start %f\n", trackState[eMEDIATYPE_AUDIO]->playTarget, trackState[eMEDIATYPE_VIDEO]->playTarget );

	return retval;
}
/***************************************************************************
* @fn Init
* @brief Function to initialize member variables,download main manifest and parse 
*		 
* @param[in] tuneType Tune type 
*
* @return bool true on success 
***************************************************************************/
AAMPStatusType StreamAbstractionAAMP_HLS::Init(TuneType tuneType)
{
	AAMPStatusType retval = eAAMPSTATUS_GENERIC_ERROR;
	bool needMetadata = true;
	mTuneType = tuneType;
	newTune = ((eTUNETYPE_NEW_NORMAL == tuneType) || (eTUNETYPE_NEW_SEEK == tuneType));

    /* START: Added As Part of DELIA-28363 and DELIA-28247 */
    aamp->IsTuneTypeNew = false;
    /* END: Added As Part of DELIA-28363 and DELIA-28247 */

	TSProcessor* audioQueuedPC = NULL;
	long http_error;

	memset(&mainManifest, 0, sizeof(mainManifest));
	if (newTune)
	{
		pthread_mutex_lock(&gDrmMutex);
		AveDrmManager::ResetAll();
		pthread_mutex_unlock(&gDrmMutex);
	}

	if (aamp->mEnableCache)
	{
		if (aamp->RetrieveFromPlaylistCache(aamp->GetManifestUrl(), &mainManifest, aamp->GetManifestUrl()))
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d Main manifest retrieved from cache\n", __FUNCTION__, __LINE__);
		}
	}
	if (!this->mainManifest.len)
	{
		aamp->profiler.ProfileBegin(PROFILE_BUCKET_MANIFEST);
		traceprintf("StreamAbstractionAAMP_HLS::%s:%d downloading manifest\n", __FUNCTION__, __LINE__);
		int manifestDLFailCount = 0;
		do
		{
			aamp->GetFile(aamp->GetManifestUrl(), &this->mainManifest, aamp->GetManifestUrl(), &http_error, NULL, 0, true, eMEDIATYPE_MANIFEST);
			if (this->mainManifest.len)
			{
				aamp->profiler.ProfileEnd(PROFILE_BUCKET_MANIFEST);
				traceprintf("StreamAbstractionAAMP_HLS::%s:%d downloaded manifest\n", __FUNCTION__, __LINE__);
				aamp->InsertToPlaylistCache(aamp->GetManifestUrl(), &mainManifest, aamp->GetManifestUrl());
				break;
			}
			logprintf("Manifest download failed : failure count : %d : http response : %d\n", manifestDLFailCount, (int) http_error);
			usleep(500000);
			manifestDLFailCount += 1;
		}
		while (MAX_MANIFEST_DOWNLOAD_RETRY > manifestDLFailCount && 404 == http_error);
	}
	if (!this->mainManifest.len && aamp->DownloadsAreEnabled()) //!aamp->GetFile(aamp->GetManifestUrl(), &this->mainManifest, aamp->GetManifestUrl()))
	{
		aamp->profiler.ProfileError(PROFILE_BUCKET_MANIFEST);
		aamp->SendDownloadErrorEvent(AAMP_TUNE_MANIFEST_REQ_FAILED, http_error);
	}
	if (this->mainManifest.len)
	{
		aamp_AppendNulTerminator(&this->mainManifest); // make safe for cstring operations
		if (gpGlobalConfig->logging.trace )
		{
			logprintf("***Main Manifest***:\n\n%s\n************\n", this->mainManifest.ptr);
		}

#ifdef AAMP_HARVEST_SUPPORT_ENABLED
		HarvestFile(aamp->GetManifestUrl(), &mainManifest, false, "main-");
#endif

		ParseMainManifest(this->mainManifest.ptr);
		if (!newTune)
		{
			long persistedBandwidth = aamp->GetPersistedBandwidth();
			//We were tuning to a lesser profile previously, so we use it as starting profile
			if (persistedBandwidth > 0 && persistedBandwidth < gpGlobalConfig->defaultBitrate)
			{
				mAbrManager.setDefaultInitBitrate(persistedBandwidth);
			}
		}
		currentProfileIndex = GetDesiredProfile(false);
		lastSelectedProfileIndex = currentProfileIndex;
		aamp->ResetCurrentlyAvailableBandwidth(this->streamInfo[this->currentProfileIndex].bandwidthBitsPerSecond,trickplayMode,this->currentProfileIndex);
		aamp->profiler.SetBandwidthBitsPerSecondVideo(this->streamInfo[this->currentProfileIndex].bandwidthBitsPerSecond);
		aamp->NotifyBitRateChangeEvent(this->streamInfo[this->currentProfileIndex].bandwidthBitsPerSecond,
						"BitrateChanged - Network Adaptation",
						this->streamInfo[this->currentProfileIndex].resolution.width,
						this->streamInfo[this->currentProfileIndex].resolution.height, true);

		/* START: Added As Part of DELIA-28363 and DELIA-28247 */
		logprintf("Selected BitRate: %ld, Max BitRate: %ld\n", streamInfo[currentProfileIndex].bandwidthBitsPerSecond, GetStreamInfo(GetMaxBWProfile())->bandwidthBitsPerSecond);
		if(newTune &&
			(streamInfo[currentProfileIndex].bandwidthBitsPerSecond < GetStreamInfo(GetMaxBWProfile())->bandwidthBitsPerSecond))
		{
			aamp->IsTuneTypeNew = newTune;
		}
		/* END: Added As Part of DELIA-28363 and DELIA-28247 */

		for (int iTrack = AAMP_TRACK_COUNT - 1; iTrack >= 0; iTrack--)
		{
			const char* trackName = "audio";
			if (eTRACK_VIDEO == iTrack)
			{
				if(trackState[eTRACK_AUDIO]->enabled)
				{
					trackName = "video";
				}
				else if (rate != AAMP_NORMAL_PLAY_RATE)
				{
					trackName = "iframe";
				}
				else
				{
					trackName = "muxed";
				}
			}
			trackState[iTrack] = new TrackState((TrackType)iTrack, this, aamp, trackName);
			TrackState *ts = trackState[iTrack];
			ts->playlistPosition = -1;
			ts->playTarget = seekPosition;
			const char *uri = GetPlaylistURI((TrackType)iTrack, &ts->streamOutputFormat);
			if (uri)
			{
				aamp_ResolveURL(ts->playlistUrl, aamp->GetManifestUrl(), uri);
				if(ts->streamOutputFormat != FORMAT_NONE)
				{
					ts->enabled = true;
					mNumberOfTracks++;
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d %s format could not be determined. codecs %s\n", __FUNCTION__, __LINE__, ts->name, streamInfo[currentProfileIndex].codecs);
				}
			}
		}

		TrackState *audio = trackState[eMEDIATYPE_AUDIO];
		TrackState *video = trackState[eMEDIATYPE_VIDEO];

        if(gpGlobalConfig->bAudioOnlyPlayback){
            if(audio->enabled){
                video->enabled = false;
                video->streamOutputFormat = FORMAT_NONE;
            }else{
                trackState[eTRACK_VIDEO]->type = eTRACK_AUDIO;
            }
        }
		aamp->profiler.SetBandwidthBitsPerSecondAudio(audio->GetCurrentBandWidth());

		pthread_t trackPLDownloadThreadID;
		bool trackPLDownloadThreadStarted = false;
		if (audio->enabled)
		{
			if (aamp->mEnableCache)
			{
				if (aamp->RetrieveFromPlaylistCache(audio->playlistUrl, &audio->playlist, audio->effectiveUrl))
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d audio playlist retrieved from cache\n", __FUNCTION__, __LINE__);
				}
			}
			if(!audio->playlist.len)
			{
				if (gpGlobalConfig->playlistsParallelFetch)
				{
					int ret = pthread_create(&trackPLDownloadThreadID, NULL, TrackPLDownloader, audio);
					if(ret != 0)
					{
						logprintf("StreamAbstractionAAMP_HLS::%s:%d pthread_create failed for TrackPLDownloader with errno = %d, %s\n", __FUNCTION__, __LINE__, errno, strerror(errno));
					}
					else
					{
						trackPLDownloadThreadStarted = true;
					}
				}
				else
				{
					audio->FetchPlaylist();
				}
			}
		}
		if (video->enabled)
		{
			if (aamp->mEnableCache)
			{
				if (aamp->RetrieveFromPlaylistCache(video->playlistUrl, &video->playlist, video->effectiveUrl))
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d video playlist retrieved from cache\n", __FUNCTION__, __LINE__);
				}
			}
			if(!video->playlist.len)
			{
				video->FetchPlaylist();
			}
		}
		if (trackPLDownloadThreadStarted)
		{
			pthread_join(trackPLDownloadThreadID, NULL);
		}
		if ((video->enabled && !video->playlist.len) || (audio->enabled && !audio->playlist.len))
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d Playlist download failed\n",__FUNCTION__,__LINE__);
			return eAAMPSTATUS_MANIFEST_DOWNLOAD_ERROR;
		}

		bool bSetStatePreparing = false;

		if (rate != AAMP_NORMAL_PLAY_RATE)
		{
			trickplayMode = true;
			if(aamp->IsTSBSupported())
			{
				mTrickPlayFPS = gpGlobalConfig->linearTrickplayFPS;
			}
			else
			{
				mTrickPlayFPS = gpGlobalConfig->vodTrickplayFPS;
			}
		}
		else
		{
			trickplayMode = false;
		}

		for (int iTrack = AAMP_TRACK_COUNT - 1; iTrack >= 0; iTrack--)
		{
			TrackState *ts = trackState[iTrack];
			aamp->SetCurlTimeout(gpGlobalConfig->fragmentDLTimeout, iTrack);

			if(ts->enabled)
			{
				bool playContextConfigured = false;
				aamp_AppendNulTerminator(&ts->playlist); // make safe for cstring operations
				if (gpGlobalConfig->logging.trace  )
				{
					logprintf("***Initial Playlist:******\n\n%s\n*****************\n", ts->playlist.ptr);
				}
#ifdef AAMP_HARVEST_SUPPORT_ENABLED
				const char* prefix = (iTrack == eTRACK_AUDIO)?"aud-":(trickplayMode)?"ifr-":"vid-";
				HarvestFile(ts->playlistUrl, &ts->playlist, false, prefix);
#endif
				ts->IndexPlaylist();
				if (ts->mDuration == 0.0f)
				{
					break;
				}

				if (newTune && needMetadata)
				{
					needMetadata = false;
					std::set<std::string> langList;
					std::vector<long> bitrateList;
					bool isIframeTrackPresent = false;
					//To avoid duplicate entries in audioLanguage list
					for (int iMedia = 0; iMedia < this->mediaCount; iMedia++)
					{
						if (this->mediaInfo[iMedia].type == eMEDIATYPE_AUDIO && this->mediaInfo[iMedia].language)
						{
							langList.insert(this->mediaInfo[iMedia].language);
						}
					}

					bitrateList.reserve(GetProfileCount());
					for (int i = 0; i < GetProfileCount(); i++)
					{
						if (!streamInfo[i].isIframeTrack)
						{
							bitrateList.push_back(streamInfo[i].bandwidthBitsPerSecond);
						}
						else
						{
							isIframeTrackPresent = true;
						}
					}
					aamp->SendMediaMetadataEvent((ts->mDuration * 1000.0), langList, bitrateList, hasDrm, isIframeTrackPresent);

					// Delay "preparing" state until all tracks have been processed.
					// JS Player assumes all onTimedMetadata event fire before "preparing" state.
					bSetStatePreparing = true;
				}

				if (iTrack == eMEDIATYPE_VIDEO)
				{
					maxIntervalBtwPlaylistUpdateMs = 2 * ts->targetDurationSeconds * 1000; //Time interval for periodic playlist update
					if (maxIntervalBtwPlaylistUpdateMs > DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS)
					{
						maxIntervalBtwPlaylistUpdateMs = DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS;
					}
					aamp->UpdateRefreshPlaylistInterval(maxIntervalBtwPlaylistUpdateMs/1000.0);
				}

				ts->fragmentURI = ts->playlist.ptr;
				StreamOutputFormat format = GetFormatFromFragmentExtension(ts);
				if (FORMAT_ISO_BMFF == format)
				{
					logprintf("StreamAbstractionAAMP_HLS::Init : Track[%s] - FORMAT_ISO_BMFF\n", ts->name);
					ts->streamOutputFormat = FORMAT_ISO_BMFF;
					continue;
				}
				if (FORMAT_AUDIO_ES_AAC == format)
				{
					logprintf("StreamAbstractionAAMP_HLS::Init : Track[%s] - FORMAT_AUDIO_ES_AAC\n", ts->name);
					ts->streamOutputFormat = FORMAT_AUDIO_ES_AAC;
					continue;
				}
				if (eMEDIATYPE_AUDIO == iTrack)
				{
					if (this->rate == AAMP_NORMAL_PLAY_RATE)
					{
						if (format == FORMAT_MPEGTS)
						{
							if (gpGlobalConfig->gAampDemuxHLSAudioTsTrack)
							{
								logprintf("Configure audio TS track demuxing\n");
								ts->playContext = new TSProcessor(aamp,eStreamOp_DEMUX_AUDIO);
							}
							else if (gpGlobalConfig->gAampMergeAudioTrack)
							{
								logprintf("Configure audio TS track to queue\n");
								ts->playContext = new TSProcessor(aamp,eStreamOp_QUEUE_AUDIO);
								ts->streamOutputFormat = FORMAT_NONE;
								audioQueuedPC = ts->playContext;
							}
							if (ts->playContext)
							{
								ts->playContext->setRate(this->rate, PlayMode_normal);
								ts->playContext->setThrottleEnable(false);
								playContextConfigured = true;
							}
							else
							{
								ts->streamOutputFormat = format;
							}
						}
						else if (FORMAT_INVALID != format)
						{
							logprintf("Configure audio format based on extension\n");
							ts->streamOutputFormat = format;
						}
						else
						{
							logprintf("Keeping audio format from playlist\n");
						}
					}
					else
					{
						logprintf("Disable audio format - trick play\n");
						ts->streamOutputFormat = FORMAT_NONE;
						ts->fragmentURI = NULL;
						ts->enabled = false;
					}
				}
				else if ((gpGlobalConfig->gAampDemuxHLSVideoTsTrack && (rate == AAMP_NORMAL_PLAY_RATE))
						|| (gpGlobalConfig->demuxHLSVideoTsTrackTM && (rate != AAMP_NORMAL_PLAY_RATE)))
				{
					HlsStreamInfo* streamInfo = &this->streamInfo[this->currentProfileIndex];
					/*Populate format from codec data*/
					format = FORMAT_INVALID;
					if (streamInfo->codecs)
					{
						for (int j = 0; j < AAMP_VIDEO_FORMAT_MAP_LEN; j++)
						{
							if (strstr(streamInfo->codecs, videoFormatMap[j].codec))
							{
								format = videoFormatMap[j].format;
								AAMPLOG_INFO("StreamAbstractionAAMP_HLS::Init : VideoTrack: format is %d [%s]\n",
									videoFormatMap[j].format, videoFormatMap[j].codec);
								break;
							}
						}
					}
					if (FORMAT_INVALID != format)
					{
						StreamOperation demuxOp;
						ts->streamOutputFormat = format;
						if ((trackState[eTRACK_AUDIO]->enabled) || (AAMP_NORMAL_PLAY_RATE != rate))
						{
							demuxOp = eStreamOp_DEMUX_VIDEO;
						}
						else
						{
							if (streamInfo->codecs)
							{
								for (int j = 0; j < AAMP_AUDIO_FORMAT_MAP_LEN; j++)
								{
									if (strstr(streamInfo->codecs, audioFormatMap[j].codec))
									{
										trackState[eMEDIATYPE_AUDIO]->streamOutputFormat = audioFormatMap[j].format;
										logprintf("StreamAbstractionAAMP_HLS::Init : Audio format is %d [%s]\n",
											audioFormatMap[j].format, audioFormatMap[j].codec);
										break;
									}
								}
							}
							if(FORMAT_NONE != trackState[eMEDIATYPE_AUDIO]->streamOutputFormat)
                            {
                                if(!gpGlobalConfig->bAudioOnlyPlayback){
                                    demuxOp = eStreamOp_DEMUX_ALL;
                                }else{
                                    demuxOp = eStreamOp_DEMUX_AUDIO;
                                    video->streamOutputFormat = FORMAT_NONE;
                                }
                            }
							else
							{
								logprintf("StreamAbstractionAAMP_HLS::%s:%d Demux only video. codecs %s\n", __FUNCTION__, __LINE__, streamInfo[currentProfileIndex].codecs);
								demuxOp = eStreamOp_DEMUX_VIDEO;
							}
						}
                        AAMPLOG_WARN("StreamAbstractionAAMP_HLS::Init : Configure video TS track demuxing demuxOp %d\n", demuxOp);
						ts->playContext = new TSProcessor(aamp,demuxOp, eMEDIATYPE_VIDEO, trackState[eMEDIATYPE_AUDIO]->playContext);
						ts->playContext->setThrottleEnable(this->enableThrottle);
						if (this->rate == AAMP_NORMAL_PLAY_RATE)
						{
							ts->playContext->setRate(this->rate, PlayMode_normal);
						}
						else
						{
							ts->playContext->setRate(this->rate, PlayMode_retimestamp_Ionly);
							ts->playContext->setFrameRateForTM(mTrickPlayFPS);
						}
						playContextConfigured = true;
					}
					else
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : VideoTrack -couldn't determine format from streamInfo->codec %s\n",
							streamInfo->codecs);
					}
				}
				else /*Video Track - No demuxing*/
				{
					if (audioQueuedPC)
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : Configure video TS track eStreamOp_SEND_VIDEO_AND_QUEUED_AUDIO\n");
						ts->playContext = new TSProcessor(aamp,eStreamOp_SEND_VIDEO_AND_QUEUED_AUDIO, eMEDIATYPE_VIDEO, audioQueuedPC);
						ts->playContext->setThrottleEnable(this->enableThrottle);
						ts->playContext->setRate(this->rate, PlayMode_normal);
						playContextConfigured = true;
					}
					else
					{
						logprintf("StreamAbstractionAAMP_HLS::Init : Configure video TS track %p : No streamops\n", ts);
					}
				}
				if (!playContextConfigured && (ts->streamOutputFormat == FORMAT_MPEGTS))
				{
					logprintf("StreamAbstractionAAMP_HLS::Init : track %p context configuring for eStreamOp_NONE\n", ts);
					ts->playContext = new TSProcessor(aamp, eStreamOp_NONE, iTrack);
					ts->playContext->setThrottleEnable(this->enableThrottle);
					if (this->rate == AAMP_NORMAL_PLAY_RATE)
					{
						this->trickplayMode = false;
						ts->playContext->setRate(this->rate, PlayMode_normal);
					}
					else
					{
						this->trickplayMode = true;
						if(aamp->IsTSBSupported())
						{
							mTrickPlayFPS = gpGlobalConfig->linearTrickplayFPS;
						}
						else
						{
							mTrickPlayFPS = gpGlobalConfig->vodTrickplayFPS;
						}
						ts->playContext->setRate(this->rate, PlayMode_retimestamp_Ionly);
						ts->playContext->setFrameRateForTM(mTrickPlayFPS);
					}
				}
			}
		}

		if ((video->enabled && video->mDuration == 0.0f) || (audio->enabled && audio->mDuration == 0.0f))
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d Track Duration is 0. Cannot play this content\n", __FUNCTION__, __LINE__);
			return eAAMPSTATUS_MANIFEST_CONTENT_ERROR;
		}

		if (bSetStatePreparing)
			aamp->SetState(eSTATE_PREPARING);

		//Currently un-used playlist indexed event, might save some JS overhead
		if (!gpGlobalConfig->disablePlaylistIndexEvent)
		{
			aamp->SendEventAsync(AAMP_EVENT_PLAYLIST_INDEXED);
		}
		if (newTune)
		{
			TunedEventConfig tunedEventConfig =  aamp->IsLive() ?
					gpGlobalConfig->tunedEventConfigLive : gpGlobalConfig->tunedEventConfigVOD;
			if (eTUNED_EVENT_ON_PLAYLIST_INDEXED == tunedEventConfig)
			{
				if (aamp->SendTunedEvent())
				{
					logprintf("aamp: hls - sent tune event after indexing playlist\n");
				}
			}
		}

		/*Do live adjust on live streams on 1. eTUNETYPE_NEW_NORMAL, 2. eTUNETYPE_SEEKTOLIVE,
		 * 3. Seek to a point beyond duration*/
		bool liveAdjust = (eTUNETYPE_NEW_NORMAL == tuneType) && (this->playlistType == ePLAYLISTTYPE_UNDEFINED) && !(aamp->IsVodOrCdvrAsset());
		if ((eTUNETYPE_SEEKTOLIVE == tuneType) && aamp->IsLive())
		{
			logprintf("StreamAbstractionAAMP_HLS::%s:%d eTUNETYPE_SEEKTOLIVE, reset playTarget and enable liveAdjust\n",__FUNCTION__,__LINE__);
			liveAdjust = true;

			audio->playTarget = 0;
			video->playTarget = 0;
			aamp->NotifyOnEnteringLive();
		}
		else if (((eTUNETYPE_SEEK == tuneType) || (eTUNETYPE_RETUNE == tuneType) || (eTUNETYPE_NEW_SEEK == tuneType)) && (this->rate > 0))
		{
			double seekWindowEnd = video->mDuration;
			if(aamp->IsLive())
			{
				seekWindowEnd -= aamp->mLiveOffset ; 
			}
			// check if seek beyond live point

			if (video->playTarget > seekWindowEnd)
			{
				if (aamp->IsLive())
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d playTarget > seekWindowEnd , playTarget:%f and seekWindowEnd:%f\n",
							__FUNCTION__,__LINE__, video->playTarget , seekWindowEnd);
					liveAdjust = true;

					audio->playTarget = 0;
					video->playTarget = 0;
					if (eTUNETYPE_SEEK == tuneType)
					{
						aamp->NotifyOnEnteringLive();
					}
				}
				else
				{
					video->eosReached = true;
					video->fragmentURI = NULL;
					audio->eosReached = true;
					audio->fragmentURI = NULL;
					logprintf("StreamAbstractionAAMP_HLS::%s:%d seek target out of range, mark EOS. playTarget:%f End:%f. \n",
							__FUNCTION__,__LINE__,video->playTarget, seekWindowEnd);

					return eAAMPSTATUS_SEEK_RANGE_ERROR;
				}
			}
		}

		if (audio->enabled)
		{

			if ( ePLAYLISTTYPE_VOD == playlistType )
			{
				SyncTracksForDiscontinuity();
			}
			else
			{
				if(!gpGlobalConfig->bAudioOnlyPlayback)
				{
					bool syncDone = false;
					if (!liveAdjust && video->mDiscontinuityIndexCount && (video->mDiscontinuityIndexCount == audio->mDiscontinuityIndexCount))
					{
						if (eAAMPSTATUS_OK == SyncTracksForDiscontinuity())
						{
							syncDone = true;
						}
					}
					if (!syncDone)
					{
						bool useProgramDateTimeIfAvailable = gpGlobalConfig->hlsAVTrackSyncUsingStartTime;
						/*For VSS streams, use program-date-time if available*/
						if (aamp->mIsVSS)
						{
							logprintf("StreamAbstractionAAMP_HLS::%s:%d : VSS stream\n", __FUNCTION__, __LINE__);
							useProgramDateTimeIfAvailable = true;
						}
						AAMPStatusType retValue = SyncTracks(useProgramDateTimeIfAvailable);
						if (eAAMPSTATUS_OK != retValue)
						{
							return retValue;
						}
					}
				}
			}
		}
		if (liveAdjust)
		{
			int offsetFromLive = aamp->mLiveOffset ; 
	
			if (video->mDuration > (offsetFromLive + video->playTargetOffset))
			{
				//DELIA-28451
				// a) Get OffSet to Live for Video and Audio separately. 
				// b) Set to minimum value among video /audio instead of setting to 0 position
				int offsetToLiveVideo,offsetToLiveAudio,offsetToLive;
				offsetToLiveVideo = offsetToLiveAudio = video->mDuration - offsetFromLive - video->playTargetOffset;
				if (audio->enabled)
				{ 
					offsetToLiveAudio = 0;
					// if audio is not having enough total duration to adjust , then offset value set to 0
					if( audio->mDuration > (offsetFromLive + audio->playTargetOffset))
						offsetToLiveAudio = audio->mDuration - offsetFromLive -  audio->playTargetOffset;
					else
						logprintf("aamp: live adjust not possible ATotal[%f]< (AoffsetFromLive[%d] + AplayTargetOffset[%f]) A-target[%f]", audio->mDuration,offsetFromLive,audio->playTargetOffset,audio->playTarget);
				}
				// pick the min of video/audio offset
				offsetToLive = (std::min)(offsetToLiveVideo,offsetToLiveAudio);
				video->playTarget += offsetToLive;
				if (audio->enabled )
				{
					audio->playTarget += offsetToLive;
				}
				// Entering live will happen if offset is adjusted , if its 0 playback is starting from beginning 
				if(offsetToLive)
					mIsAtLivePoint = true;
				logprintf("aamp: after live adjust - V-target %f A-target %f offsetFromLive %d offsetToLive %d offsetVideo[%d] offsetAudio[%d] AtLivePoint[%d]\n",
				        video->playTarget, audio->playTarget, offsetFromLive, offsetToLive,offsetToLiveVideo,offsetToLiveAudio,mIsAtLivePoint);
			}
			else
			{
				logprintf("aamp: live adjust not possible VTotal[%f] < (VoffsetFromLive[%d] + VplayTargetOffset[%f]) V-target[%f]", 
					video->mDuration,offsetFromLive,video->playTargetOffset,video->playTarget);
			}
			//Set live adusted position to seekPosition
			seekPosition = video->playTarget;
		}
		/*Adjust for discontinuity*/
		if ((audio->enabled) && (ePLAYLISTTYPE_VOD != playlistType) && !gpGlobalConfig->bAudioOnlyPlayback)
		{
			int discontinuityIndexCount = video->mDiscontinuityIndexCount;
			if (discontinuityIndexCount > 0)
			{
				if (discontinuityIndexCount == audio->mDiscontinuityIndexCount)
				{
					if (liveAdjust)
					{
						SyncTracksForDiscontinuity();
					}
					float videoPrevDiscontinuity = 0;
					float audioPrevDiscontinuity = 0;
					float videoNextDiscontinuity;
					float audioNextDiscontinuity;
					DiscontinuityIndexNode* videoDiscontinuityIndex = (DiscontinuityIndexNode*)video->mDiscontinuityIndex.ptr;
					DiscontinuityIndexNode* audioDiscontinuityIndex = (DiscontinuityIndexNode*)audio->mDiscontinuityIndex.ptr;
					for (int i = 0; i <= discontinuityIndexCount; i++)
					{
						if (i < discontinuityIndexCount)
						{
							videoNextDiscontinuity = videoDiscontinuityIndex[i].position;
							audioNextDiscontinuity = audioDiscontinuityIndex[i].position;
						}
						else
						{
							videoNextDiscontinuity = aamp->GetDurationMs() / 1000;
							audioNextDiscontinuity = videoNextDiscontinuity;
						}
						if ((videoNextDiscontinuity > (video->playTarget + 5))
						        && (audioNextDiscontinuity > (audio->playTarget + 5)))
						{

							logprintf( "StreamAbstractionAAMP_HLS::%s:%d : video->playTarget %f videoPrevDiscontinuity %f videoNextDiscontinuity %f\n",
									__FUNCTION__, __LINE__, video->playTarget, videoPrevDiscontinuity, videoNextDiscontinuity);
							logprintf( "StreamAbstractionAAMP_HLS::%s:%d : audio->playTarget %f audioPrevDiscontinuity %f audioNextDiscontinuity %f\n",
									__FUNCTION__, __LINE__, audio->playTarget, audioPrevDiscontinuity, audioNextDiscontinuity);
							if (video->playTarget < videoPrevDiscontinuity)
							{
								logprintf( "StreamAbstractionAAMP_HLS::%s:%d : [video] playTarget(%f) advance to discontinuity(%f)\n",
										__FUNCTION__, __LINE__, video->playTarget, videoPrevDiscontinuity);
								video->playTarget = videoPrevDiscontinuity;
							}
							if (audio->playTarget < audioPrevDiscontinuity)
							{
								logprintf( "StreamAbstractionAAMP_HLS::%s:%d : [audio] playTarget(%f) advance to discontinuity(%f)\n",
										__FUNCTION__, __LINE__, audio->playTarget, audioPrevDiscontinuity);
								audio->playTarget = audioPrevDiscontinuity;
							}
							break;
						}
						videoPrevDiscontinuity = videoNextDiscontinuity;
						audioPrevDiscontinuity = audioNextDiscontinuity;
					}
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d : videoPeriodPositionIndex.size %d audioPeriodPositionIndex.size %d\n",
							__FUNCTION__, __LINE__, video->mDiscontinuityIndexCount, audio->mDiscontinuityIndexCount);
				}
			}
			else
			{
				logprintf("StreamAbstractionAAMP_HLS::%s:%d : videoPeriodPositionIndex.size 0\n", __FUNCTION__, __LINE__);
			}
		}
		if (audio->enabled)
		{
			audio->ProcessDrmMetadata(true);
		}
		if (video->enabled)
		{
			video->ProcessDrmMetadata(true);
		}

		audio->lastPlaylistDownloadTimeMS = aamp_GetCurrentTimeMS();
		video->lastPlaylistDownloadTimeMS = audio->lastPlaylistDownloadTimeMS;
		/*Use start timestamp as zero when audio is not elementary stream*/
		mStartTimestampZero = ((rate == AAMP_NORMAL_PLAY_RATE) && ((!audio->enabled) || audio->playContext));
		if (!aamp->mEnableCache)
		{
			aamp->ClearPlaylistCache();
		}
		else if (newTune && gpGlobalConfig->prefetchIframePlaylist)
		{
			int iframeStreamIdx = GetIframeTrack();
			if (0 <= iframeStreamIdx)
			{
				char defaultIframePlaylistUrl[MAX_URI_LENGTH];
				char defaultIframePlaylistEffectiveUrl[MAX_URI_LENGTH];
				GrowableBuffer defaultIframePlaylist;
				aamp_ResolveURL(defaultIframePlaylistUrl, aamp->GetManifestUrl(), streamInfo[iframeStreamIdx].uri);
				traceprintf("StreamAbstractionAAMP_HLS::%s:%d : Downloading iframe playlist\n", __FUNCTION__, __LINE__);
				aamp->GetFile(defaultIframePlaylistUrl, &defaultIframePlaylist, defaultIframePlaylistEffectiveUrl, &http_error);
				if (defaultIframePlaylist.len)
				{
					aamp->InsertToPlaylistCache(defaultIframePlaylistUrl, &defaultIframePlaylist, defaultIframePlaylistEffectiveUrl);
					traceprintf("StreamAbstractionAAMP_HLS::%s:%d : Cached iframe playlist\n", __FUNCTION__, __LINE__);
				}
				else
				{
					logprintf("StreamAbstractionAAMP_HLS::%s:%d : Error Download iframe playlist. http_error %ld\n",
					        __FUNCTION__, __LINE__, http_error);
				}
			}
		}
		retval = eAAMPSTATUS_OK;
	}
	return retval;
}
/***************************************************************************
* @fn GetFirstPTS
* @brief Function to return first PTS 
*		 
* @return double PTS value 
***************************************************************************/
double StreamAbstractionAAMP_HLS::GetFirstPTS()
{
	double pts;
	if( mStartTimestampZero)
	{
		pts = 0;
	}
	else
	{
		pts = seekPosition;
	}
	return pts;
}

/***************************************************************************
* @fn RunFetchLoop
* @brief Fragment collector thread execution function to download fragments 
*		 
* @return void
***************************************************************************/
void TrackState::RunFetchLoop()
{
	for (;;)
	{
		while (fragmentURI && aamp->DownloadsAreEnabled())
		{
			traceprintf("%s:%d mInjectInitFragment %d mInitFragmentInfo %p\n",
					__FUNCTION__, __LINE__, (int)mInjectInitFragment, mInitFragmentInfo);
			if (mInjectInitFragment && mInitFragmentInfo)
			{
				long http_code = -1;
				ProfilerBucketType bucketType = aamp->GetProfilerBucketForMedia((MediaType)type, true);
				aamp->profiler.ProfileBegin(bucketType);
				if(FetchInitFragment(http_code))
				{
					aamp->profiler.ProfileEnd(bucketType);
					mInjectInitFragment = false;
				}
				else
				{
					logprintf("%s:%d Init fragment fetch failed\n", __FUNCTION__, __LINE__);
					aamp->profiler.ProfileError(bucketType);
					aamp->SendDownloadErrorEvent(AAMP_TUNE_INIT_FRAGMENT_DOWNLOAD_FAILURE, http_code);
				}
			}
			FetchFragment();

			// FetchFragment involves multiple wait operations, so check download status again
			if (!aamp->DownloadsAreEnabled())
			{
				break;
			}

			traceprintf("%s:%d: gDeferredDrmLicTagUnderProcessing %d gDeferredDrmLicRequestPending %d\n", __FUNCTION__, __LINE__, (int)gDeferredDrmLicTagUnderProcessing, (int)gDeferredDrmLicRequestPending);
			pthread_mutex_lock(&gDrmMutex);
			if(gDeferredDrmLicTagUnderProcessing && gDeferredDrmLicRequestPending)
			{
				if(aamp_GetCurrentTimeMS() > gDeferredDrmTime)
				{
					StartDeferredDrmLicenseAcquisition();
				}
			}
			pthread_mutex_unlock(&gDrmMutex);

			if (mDrmLicenseRequestPending)
			{
				logprintf("%s:%d: Start acquisition of pending DRM licenses\n", __FUNCTION__, __LINE__);
				ProcessDrmMetadata(false);
			}

			/*Check for profile change only for video track*/
			if((eTRACK_VIDEO == type) && (!context->trickplayMode))
			{
				context->lastSelectedProfileIndex = context->currentProfileIndex;
				//DELIA-33346 -- if rampdown is attempted to any failure , no abr change to be attempted . 
				// else profile be resetted to top one leading to looping of bad fragment 
				if(!context->mCheckForRampdown)
				{
					if (aamp->CheckABREnabled())
					{
						context->CheckForProfileChange();
					}
					else if (!context->aamp->IsTSBSupported())
					{
						context->CheckUserProfileChangeReq();
					}
				}
			}

			if (ePLAYLISTTYPE_VOD != context->playlistType)
			{
				int timeSinceLastPlaylistDownload = (int) (aamp_GetCurrentTimeMS()
				        - lastPlaylistDownloadTimeMS);
				if (context->maxIntervalBtwPlaylistUpdateMs <= timeSinceLastPlaylistDownload)
				{
					AAMPLOG_INFO("%s:%d: Refreshing playlist as maximum refresh delay exceeded\n", __FUNCTION__, __LINE__);
					RefreshPlaylist();
				}
#ifdef TRACE
				else
				{
					logprintf("%s:%d: Not refreshing timeSinceLastPlaylistDownload = %d\n", __FUNCTION__, __LINE__, timeSinceLastPlaylistDownload);
				}
#endif
			}
			pthread_mutex_lock(&mutex);
			if(refreshPlaylist)
			{
				RefreshPlaylist();
				refreshPlaylist = false;
			}
			pthread_mutex_unlock(&mutex);
		}
		// reached end of vod stream
		//teststreamer_EndOfStreamReached();

		if (eosReached || context->hasEndListTag || !context->aamp->DownloadsAreEnabled())
		{
			AbortWaitForCachedFragment(false);
			break;
		}
		if (lastPlaylistDownloadTimeMS)
		{
			// if not present, new playlist wih at least one additional segment will be available
			// no earlier than 0.5*EXT-TARGETDURATION and no later than 1.5*EXT-TARGETDURATION
			// relative to previous playlist fetch.
			int timeSinceLastPlaylistDownload = (int)(aamp_GetCurrentTimeMS() - lastPlaylistDownloadTimeMS);
			int minDelayBetweenPlaylistUpdates = MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			long long currentPlayPosition = aamp->GetPositionMs();
			long long endPositionAvailable = (aamp->culledSeconds + aamp->durationSeconds)*1000;
			// playTarget value will vary if TSB is full and trickplay is attempted. Cant use for buffer calculation 
			// So using the endposition in playlist - Current playing position to get the buffer availability
			long bufferAvailable = (endPositionAvailable - currentPlayPosition);
			// If buffer Available is > 2*targetDuration
			if(bufferAvailable  > (targetDurationSeconds*2*1000) )
			{
				// may be 1.0 times also can be set ??? 
				minDelayBetweenPlaylistUpdates = (int)(1.5 * 1000 * targetDurationSeconds);
			}
			// if buffer is between 2*target & targetDuration
			else if(bufferAvailable  > (targetDurationSeconds*1000))
			{
				minDelayBetweenPlaylistUpdates = (int)(0.5 * 1000 * targetDurationSeconds);
			}
			// This is to handle the case where target duration is high value(>Max delay)  but buffer is available just above the max update inteval
			else if(bufferAvailable > (2*MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS))
			{
				minDelayBetweenPlaylistUpdates = MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			}
			// if buffer < targetDuration && buffer < MaxDelayInterval
			else
			{
				// if bufferAvailable is less than targetDuration ,its in RED alert . Close to freeze 
				// need to refresh soon ..
				if(bufferAvailable)
				{
					minDelayBetweenPlaylistUpdates = (int)(bufferAvailable / 3) ; 
				}
				else
				{
					minDelayBetweenPlaylistUpdates = MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS; // 500mSec
				}
				// limit the logs when buffer is low 
				{
					static int bufferlowCnt;
					if((bufferlowCnt++ & 5) == 0)
					{ 
						logprintf("%s:%d: Buffer is running low(%ld).Refreshing playlist(%d).Target(%f) PlayPosition(%lld) End(%lld)\n",
							__FUNCTION__, __LINE__, bufferAvailable,minDelayBetweenPlaylistUpdates,playTarget,currentPlayPosition,endPositionAvailable);
					}
				}
			}
			// adjust with last refreshed time interval
			minDelayBetweenPlaylistUpdates -= timeSinceLastPlaylistDownload;
			// restrict to Max delay interval 	
			if (minDelayBetweenPlaylistUpdates > MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS)
			{
				minDelayBetweenPlaylistUpdates = MAX_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			}
			else if(minDelayBetweenPlaylistUpdates < MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS) 
			{
				// minimum of 500 mSec needed to avoid too frequent download.
				minDelayBetweenPlaylistUpdates = MIN_DELAY_BETWEEN_PLAYLIST_UPDATE_MS;
			}
			AAMPLOG_INFO("%s:%d: aamp playlist end refresh bufferMs(%ld) playtarget(%f) delay(%d) End(%lld) PlayPosition(%lld)\n",
					__FUNCTION__, __LINE__, bufferAvailable,playTarget,minDelayBetweenPlaylistUpdates,endPositionAvailable,currentPlayPosition);
			aamp->InterruptableMsSleep(minDelayBetweenPlaylistUpdates);
		}
		RefreshPlaylist();

		AAMPLOG_FAILOVER("%s:%d: fragmentURI [%s] timeElapsedSinceLastFragment [%f]\n",
			__FUNCTION__, __LINE__, fragmentURI, (aamp_GetCurrentTimeMS() - context->LastVideoFragParsedTimeMS()));

		/* Added to handle an edge case for cdn failover, where we found valid sub-manifest but no valid fragments.
		 * In this case we have to stall the playback here. */
		if( fragmentURI == NULL && aamp->IsLive() && type == eTRACK_VIDEO)
		{
			AAMPLOG_FAILOVER("%s:%d: fragmentURI is NULL, playback may stall in few seconds..\n", __FUNCTION__, __LINE__);
			context->CheckForPlaybackStall(false);
		}
	}
	AAMPLOG_WARN("%s:%d: fragment collector done. track %s\n", __FUNCTION__, __LINE__, name);
}
/***************************************************************************
* @fn FragmentCollector
* @brief Fragment collector thread function
*		 
* @param arg[in] TrackState pointer
* @return void
***************************************************************************/

static void *FragmentCollector(void *arg)
{
	TrackState *track = (TrackState *)arg;
	if(aamp_pthread_setname(pthread_self(), "aampHLSFetch"))
	{
		logprintf("%s:%d: aamp_pthread_setname failed\n", __FUNCTION__, __LINE__);
	}
	track->RunFetchLoop();
	return NULL;
}
/***************************************************************************
* @fn StreamAbstractionAAMP_HLS
* @brief Constructor function 
*		 
* @param[in] aamp            PrivateInstanceAAMP pointer
* @param[in] seekpos         Seek position 
* @param[in] rate            Rate of playback 
* @param[in] enableThrottle  Throttle enable/disable flag
*
* @return void
***************************************************************************/
StreamAbstractionAAMP_HLS::StreamAbstractionAAMP_HLS(class PrivateInstanceAAMP *aamp,double seekpos, float rate, bool enableThrottle) : StreamAbstractionAAMP(aamp)
{
#ifndef AVE_DRM
       logprintf("PlayerInstanceAAMP() : AVE DRM disabled\n");
#endif
	trickplayMode = false;

	maxIntervalBtwPlaylistUpdateMs = DEFAULT_INTERVAL_BETWEEN_PLAYLIST_UPDATES_MS;

	this->seekPosition = seekpos;
	logprintf("hls fragment collector seekpos = %f\n", seekpos);
	this->rate = rate;
	if (rate == AAMP_NORMAL_PLAY_RATE)
	{
		this->trickplayMode = false;
	}
	else
	{
		this->trickplayMode = true;
	}
	this->enableThrottle = enableThrottle;
	firstFragmentDecrypted = false;

	playlistType = ePLAYLISTTYPE_UNDEFINED;
	hasEndListTag = false;
	//targetDurationSeconds = 0.0;
	mAbrManager.clearProfiles();
	mediaCount = 0;
	allowsCache = false;
	newTune = true;
	segDLFailCount = 0;
	memset(&trackState[0], 0x00, sizeof(trackState));
	mStartTimestampZero = false;
	aamp->CurlInit(0, AAMP_TRACK_COUNT);
	lastSelectedProfileIndex = 0;
	mNumberOfTracks = 0;
}
/***************************************************************************
* @fn TrackState
* @brief TrackState Constructor
*		 
* @param[in] type    Type of the track
* @param[in] parent  StreamAbstractionAAMP_HLS instance 
* @param[in] aamp    PrivateInstanceAAMP pointer 
* @param[in] name    Name of the track 
*
* @return void
***************************************************************************/
TrackState::TrackState(TrackType type, StreamAbstractionAAMP_HLS* parent, PrivateInstanceAAMP* aamp, const char* name) :
		MediaTrack(type, aamp, name),
		indexCount(0), currentIdx(0), indexFirstMediaSequenceNumber(0), fragmentURI(NULL), lastPlaylistDownloadTimeMS(0),
		byteRangeLength(0), byteRangeOffset(0), nextMediaSequenceNumber(0), playlistPosition(0), playTarget(0),
		streamOutputFormat(FORMAT_NONE), playContext(NULL),
		playTargetOffset(0),
		discontinuity(false),
		refreshPlaylist(false), fragmentCollectorThreadID(0),
		fragmentCollectorThreadStarted(false),
		manifestDLFailCount(0),
		mCMSha1Hash(NULL), mDrmTimeStamp(0), mDrmMetaDataIndexCount(0),firstIndexDone(false), mDrm(NULL), mDrmLicenseRequestPending(false),
		mInjectInitFragment(true), mInitFragmentInfo(NULL), mDrmKeyTagCount(0), mIndexingInProgress(false), mForceProcessDrmMetadata(false),
		mDuration(0), mLastMatchedDiscontPosition(-1), mCulledSeconds(0),
		mDiscontinuityIndexCount(0), mSyncAfterDiscontinuityInProgress(false)
{
	this->context = parent;
	targetDurationSeconds = 1; // avoid tight loop

	effectiveUrl[0] = 0,
	playlistUrl[0] = 0;
	memset(&playlist, 0, sizeof(playlist));
	memset(&index, 0, sizeof(index));
	fragmentURIFromIndex[0] = 0;
	memset(&startTimeForPlaylistSync, 0, sizeof(struct timeval));
	fragmentEncrypted = false;
	memset(&mDrmMetaDataIndex, 0, sizeof(mDrmMetaDataIndex));
	memset(&mDrmInfo, 0, sizeof(mDrmInfo));
	mDrmMetaDataIndexPosition = 0;
	memset(&mDiscontinuityIndex, 0, sizeof(mDiscontinuityIndex));
	pthread_cond_init(&mPlaylistIndexed, NULL);
	pthread_mutex_init(&mPlaylistMutex, NULL);
}
/***************************************************************************
* @fn ~TrackState
* @brief Destructor function 
*		 
* @return void
***************************************************************************/
TrackState::~TrackState()
{
	aamp_Free(&playlist.ptr);
	for (int j=0; j< gpGlobalConfig->maxCachedFragmentsPerTrack; j++)
	{
		aamp_Free(&cachedFragment[j].fragment.ptr);
	}
	FlushIndex();
	if (playContext)
	{
		delete playContext;
	}
	if (mCMSha1Hash)
	{
		free(mCMSha1Hash);
	}
	if (mDrmInfo.iv)
	{
		free(mDrmInfo.iv);
	}
	if (mDrmInfo.uri)
	{
		free(mDrmInfo.uri);
	}
	pthread_cond_destroy(&mPlaylistIndexed);
	pthread_mutex_destroy(&mPlaylistMutex);
}
/***************************************************************************
* @fn Stop
* @brief Function to stop track download/playback 
*		 
* @param clearDRM[in] flag indicating if DRM resources to be freed or not
* @return void
***************************************************************************/
void TrackState::Stop(bool clearDRM)
{
	AbortWaitForCachedFragment(true);

	if (mDrm)
	{
		//To force release gDrmMutex mutex held by drm_Decrypt in case of clearDRM
		mDrm->CancelKeyWait();

		if(clearDRM && aamp->GetCurrentDRM() != eDRM_Adobe_Access)
		{
			pthread_mutex_lock(&gDrmMutex);
			mDrm->Release();
			pthread_mutex_unlock(&gDrmMutex);
		}
	}

	if (playContext)
	{
		playContext->abort();
	}
	if (fragmentCollectorThreadStarted)
	{
		void *value_ptr = NULL;
		int rc = pthread_join(fragmentCollectorThreadID, &value_ptr);
		if (rc != 0)
		{
			logprintf("***pthread_join fragmentCollectorThread returned %d(%s)\n", rc, strerror(rc));
		}
#ifdef TRACE
		else
		{
			logprintf("joined fragmentCollectorThread\n");
		}
#endif
		fragmentCollectorThreadStarted = false;
	}
	StopInjectLoop();

	if (!clearDRM && mDrm)
	{
		//Restore drm key state which was reset by drm_CancelKeyWait earlier since drm data is persisted
		mDrm->RestoreKeyState();
	}
}
/***************************************************************************
* @fn ~StreamAbstractionAAMP_HLS
* @brief Destructor function for StreamAbstractionAAMP_HLS
*		 
* @return void
***************************************************************************/
StreamAbstractionAAMP_HLS::~StreamAbstractionAAMP_HLS()
{
	/*Exit from ongoing  http fetch, drm operation,throttle. Mark fragment collector exit*/

	for (int i = 0; i < AAMP_TRACK_COUNT; i++)
	{
		TrackState *track = trackState[i];
		if (track)
		{
			delete track;
		}
	}

	aamp->SyncBegin();
	aamp_Free(&this->mainManifest.ptr);
	aamp->CurlTerm(0, AAMP_TRACK_COUNT);
	aamp->SyncEnd();
}
/***************************************************************************
* @fn Start
* @brief Function to create threads for track donwload
*		 
* @return void
***************************************************************************/
void TrackState::Start(void)
{
	if(playContext)
	{
		playContext->reset();
	}
	assert(!fragmentCollectorThreadStarted);
	if (0 == pthread_create(&fragmentCollectorThreadID, NULL, &FragmentCollector, this))
	{
		fragmentCollectorThreadStarted = true;
	}
	else
	{
		logprintf("Failed to create FragmentCollector thread\n");
	}
	mInjectInitFragment = true;
	StartInjectLoop();
}
/***************************************************************************
* @fn Start
* @brief Function to start track initiaziation 
*		 
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::Start(void)
{
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track->Enabled())
		{
			track->Start();
		}
	}
}
/***************************************************************************
* @fn Stop
* @brief Function to stop the HLS streaming 
*		 
* @param[in] clearChannelData Flag indicating to full stop or temporary stop	
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::Stop(bool clearChannelData)
{
	aamp->DisableDownloads();
	ReassessAndResumeAudioTrack(true);
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];

		/*Stop any waits for other track's playlist update*/
		TrackState *otherTrack = trackState[(iTrack == eTRACK_VIDEO)? eTRACK_AUDIO: eTRACK_VIDEO];
		if(otherTrack && otherTrack->Enabled())
		{
			otherTrack->StopWaitForPlaylistRefresh();
		}

		if(track && track->Enabled())
		{
			track->Stop(clearChannelData);
		}
	}

	if (clearChannelData && aamp->GetCurrentDRM() == eDRM_Adobe_Access)
	{
		pthread_mutex_lock(&gDrmMutex);
		AveDrmManager::CancelKeyWaitAll();
		AveDrmManager::ReleaseAll();
		AveDrmManager::ResetAll();
		gDeferredDrmLicRequestPending = false;
		gDeferredDrmLicTagUnderProcessing = false;
		pthread_mutex_unlock(&gDrmMutex);
	}

	aamp->EnableDownloads();
}
/***************************************************************************
* @fn DumpProfiles
* @brief Function to log debug information on Stream/Media information
*		 
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::DumpProfiles(void)
{
	int profileCount = GetProfileCount();
	if (profileCount)
	{
		for (int i = 0; i < profileCount; i++)
		{
			struct HlsStreamInfo *streamInfo = &this->streamInfo[i];
			logprintf("stream[%d]:\n", i);
			if (streamInfo->uri) logprintf("\tURI:%s\n", streamInfo->uri);
			logprintf("\tBANDWIDTH:%ld\n", streamInfo->bandwidthBitsPerSecond);
			logprintf("\tPROGRAM-ID:%ld\n", streamInfo->program_id);
			if (streamInfo->audio) logprintf("\tAUDIO:%s\n", streamInfo->audio);
			if (streamInfo->codecs) logprintf("\tCODECS:%s\n", streamInfo->codecs);
			logprintf("\tRESOLUTION: %dx%d\n", streamInfo->resolution.width, streamInfo->resolution.height);
		}
		logprintf("\n");
	}

	if (this->mediaCount)
	{
		for (int i = 0; i < this->mediaCount; i++)
		{
			MediaInfo *mediaInfo = &this->mediaInfo[i];
			logprintf("media[%d]:\n", i);
			if (mediaInfo->uri) logprintf("\tURI:%s\n", mediaInfo->uri);
			switch (mediaInfo->type)
			{
			case eMEDIATYPE_AUDIO:
				logprintf("type:AUDIO\n");
				break;
			case eMEDIATYPE_VIDEO:
				logprintf("type:VIDEO\n");
				break;
			default:
				break;
			}
			if (mediaInfo->group_id) logprintf("\tgroup-id:%s\n", mediaInfo->group_id);
			if (mediaInfo->name) logprintf("\tname:%s\n", mediaInfo->name);
			if (mediaInfo->language) logprintf("\tlanguage:%s\n", mediaInfo->language);
			if (mediaInfo->autoselect)
			{
				logprintf("\tAUTOSELECT\n");
			}
			if (mediaInfo->isDefault)
			{
				logprintf("\tDEFAULT\n");
			}
		}
		logprintf("\n");
	}
}

/***************************************************************************
* @fn GetStreamFormat
* @brief Function to get stream format 
*		 
* @param[out]  primaryOutputFormat  Video format 
* @param[out]  audioOutputFormat    Audio format
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::GetStreamFormat(StreamOutputFormat &primaryOutputFormat, StreamOutputFormat &audioOutputFormat)
{
	primaryOutputFormat = trackState[eMEDIATYPE_VIDEO]->streamOutputFormat;
	audioOutputFormat = trackState[eMEDIATYPE_AUDIO]->streamOutputFormat;
}
/***************************************************************************
* @fn GetVideoBitrates
* @brief Function to get available video bitrates
*
* @return available video bitrates
***************************************************************************/
std::vector<long> StreamAbstractionAAMP_HLS::GetVideoBitrates(void)
{
	std::vector<long> bitrates;
	int profileCount = GetProfileCount();
	if (profileCount)
	{
		for (int i = 0; i < profileCount; i++)
		{
			struct HlsStreamInfo *streamInfo = &this->streamInfo[i];
			//Not send iframe bw info, since AAMP has ABR disabled for trickmode
			if (!streamInfo->isIframeTrack)
			{
				bitrates.push_back(streamInfo->bandwidthBitsPerSecond);
			}
		}
	}
	return bitrates;
}
/***************************************************************************
* @fn GetAudioBitrates
* @brief Function to get available audio bitrates
*
* @return available audio bitrates
***************************************************************************/
std::vector<long> StreamAbstractionAAMP_HLS::GetAudioBitrates(void)
{
	//TODO: Impl audio bitrate getter
	return std::vector<long>();
}
/***************************************************************************
* @fn DrmDecrypt
* @brief Function to decrypt the fragment for playback
*		 
* @param[in] cachedFragment                CachedFragment struction pointer 	
* @param[in] bucketTypeFragmentDecrypt     ProfilerBucketType enum
*
* @return bool true if successfully decrypted 
***************************************************************************/
DrmReturn TrackState::DrmDecrypt( CachedFragment * cachedFragment, ProfilerBucketType bucketTypeFragmentDecrypt)
{
		DrmReturn drmReturn = eDRM_ERROR;
		if (aamp->DownloadsAreEnabled())
		{
			drmReturn = eDRM_ERROR;
			pthread_mutex_lock(&gDrmMutex);
			bool isVanilaAES = ((eMETHOD_AES_128 ==mDrmInfo.method ) && ( 0 == mDrmMetaDataIndexCount));
			if (!mDrm || (mCMSha1Hash) || isVanilaAES)
			{
				SetDrmContextUnlocked();
			}
			else if ((eMETHOD_AES_128 ==mDrmInfo.method ) && ( 0 == mDrmMetaDataIndexCount))
			{
				SetDrmContextUnlocked();
			}
			if(mDrm)
			{
				drmReturn = mDrm->Decrypt(bucketTypeFragmentDecrypt, cachedFragment->fragment.ptr,
						cachedFragment->fragment.len, MAX_LICENSE_ACQ_WAIT_TIME);

			}
			pthread_mutex_unlock(&gDrmMutex);
		}

		if (drmReturn != eDRM_SUCCESS)
		{
			aamp->profiler.ProfileError(bucketTypeFragmentDecrypt, drmReturn);
		}
		return drmReturn;
}
/***************************************************************************
* @fn GetContext
* @brief Function to get current StreamAbstractionAAMP instance value 
*		 
* @return StreamAbstractionAAMP instance
***************************************************************************/
StreamAbstractionAAMP* TrackState::GetContext()
{
	return context;
}
/***************************************************************************
* @fn GetMediaTrack
* @brief Function to get Media information for track type
*		 
* @param[in] type  TrackType input	
*
* @return MediaTrack structure pointer
***************************************************************************/
MediaTrack* StreamAbstractionAAMP_HLS::GetMediaTrack(TrackType type)
{
	return trackState[(int)type];
}
/***************************************************************************
* @fn SetDrmContextUnlocked
* @brief Function to set DRM Context value based on DRM Metadata 
*		 
* @return void
***************************************************************************/
void TrackState::SetDrmContextUnlocked()
{
	bool drmContextUpdated = false;
	DrmMetadataNode* drmMetadataIdx = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
	DrmMetadata *drmMetadata = NULL;
	if(mDrmMetaDataIndexCount > 0)
	{
		drmMetadata = &drmMetadataIdx[mDrmMetaDataIndexPosition].metaData;
		assert(drmMetadata->metadataPtr);
	}
	traceprintf("TrackState::%s Enter mCMSha1Hash %p mDrmMetaDataIndexPosition %d\n", __FUNCTION__, mCMSha1Hash,
	        mDrmMetaDataIndexPosition);

	if (drmMetadata)
	{
		mDrm = AveDrmManager::GetAveDrm(drmMetadataIdx[mDrmMetaDataIndexPosition].sha1Hash);
		if (!mDrm)
		{
			logprintf("%s:%d [%s] GetAveDrm failed\n", __FUNCTION__, __LINE__, name);
			printf("%s:%d [%s] sha1hash - ", __FUNCTION__, __LINE__, name);
			AveDrmManager::PrintSha1Hash(drmMetadataIdx[mDrmMetaDataIndexPosition].sha1Hash);
			if(gDeferredDrmLicTagUnderProcessing && gDeferredDrmLicRequestPending)
			{
				logprintf("%s:%d [%s] GetAveDrm failed\n", __FUNCTION__, __LINE__, name);
				StartDeferredDrmLicenseAcquisition();
				mDrm = AveDrmManager::GetAveDrm(drmMetadataIdx[mDrmMetaDataIndexPosition].sha1Hash);
			}
			if (!mDrm)
			{
				if (mDrmLicenseRequestPending)
				{
					pthread_mutex_unlock(&gDrmMutex);
					logprintf("%s:%d: Start acquisition of pending DRM licenses\n", __FUNCTION__, __LINE__);
					ProcessDrmMetadata(false);
					pthread_mutex_lock(&gDrmMutex);
					mDrm = AveDrmManager::GetAveDrm(drmMetadataIdx[mDrmMetaDataIndexPosition].sha1Hash);
				}
			}
			if (!mDrm)
			{
				printf("%s:%d [%s] GetAveDrm failed for sha1hash - ", __FUNCTION__, __LINE__, name);
				AveDrmManager::PrintSha1Hash(drmMetadataIdx[mDrmMetaDataIndexPosition].sha1Hash);
				AveDrmManager::DumpCachedLicenses();
			}
		}
	}
	else
	{
#ifdef AAMP_VANILLA_AES_SUPPORT
		AAMPLOG_INFO("StreamAbstractionAAMP_HLS::%s:%d Get AesDec\n", __FUNCTION__, __LINE__);
		mDrm = AesDec::GetInstance();
#else
		logprintf("StreamAbstractionAAMP_HLS::%s:%d AAMP_VANILLA_AES_SUPPORT not defined\n", __FUNCTION__, __LINE__);
#endif
	}
	if(mDrm)
	{
		mDrm->SetDecryptInfo(aamp, &mDrmInfo);
	}
}
/***************************************************************************
* @fn UpdateDrmCMSha1Hash
* @brief Function to Update SHA1 Id for DRM Metadata 
*		 
* @param[in] ptr ShaID string from DRM attribute
*
* @return void
***************************************************************************/
void TrackState::UpdateDrmCMSha1Hash(const char *ptr)
{
	bool drmDataChanged = false;
	if (NULL == ptr)
	{
		if (mCMSha1Hash)
		{
			free(mCMSha1Hash);
			mCMSha1Hash = NULL;
		}
	}
	else if (mCMSha1Hash)
	{
		if (0 != memcmp(ptr, (char*) mCMSha1Hash, DRM_SHA1_HASH_LEN))
		{
			if (!mIndexingInProgress)
			{
				printf("%s:%d [%s] Different DRM metadata hash. old - ", __FUNCTION__, __LINE__, name);
				for (int i = 0; i< DRM_SHA1_HASH_LEN; i++)
				{
					printf("%c", mCMSha1Hash[i]);
				}
				printf(" new - ");
				for (int i = 0; i< DRM_SHA1_HASH_LEN; i++)
				{
					printf("%c", ptr[i]);
				}
				printf("\n");
			}
			drmDataChanged = true;
			memcpy(mCMSha1Hash, ptr, DRM_SHA1_HASH_LEN);
		}
		else
		{
			AAMPLOG_INFO("%s:%d Same DRM Metadata\n", __FUNCTION__, __LINE__);
		}
	}
	else
	{
		if (!mIndexingInProgress)
		{
			printf("%s:%d [%s] New DRM metadata hash - ", __FUNCTION__, __LINE__, name);
			for (int i = 0; i < DRM_SHA1_HASH_LEN; i++)
			{
				printf("%c", ptr[i]);
			}
			printf("\n");
		}
		mCMSha1Hash = (char*)malloc(DRM_SHA1_HASH_LEN);
		memcpy(mCMSha1Hash, ptr, DRM_SHA1_HASH_LEN);
		drmDataChanged = true;
	}
	if(drmDataChanged)
	{
		int i;
		DrmMetadataNode* drmMetadataNode = (DrmMetadataNode*)mDrmMetaDataIndex.ptr;
		for (i = 0; i < mDrmMetaDataIndexCount; i++)
		{
			if(drmMetadataNode[i].sha1Hash)
			{
				if (0 == memcmp(mCMSha1Hash, drmMetadataNode[i].sha1Hash, DRM_SHA1_HASH_LEN))
				{
					AAMPLOG_INFO("%s:%d mDrmMetaDataIndexPosition %d->%d\n", __FUNCTION__, __LINE__, mDrmMetaDataIndexPosition, i);
					mDrmMetaDataIndexPosition = i;
					break;
				}
			}
		}
		if (i == mDrmMetaDataIndexCount)
		{
			logprintf("%s:%d [%s] Couldn't find matching hash mDrmMetaDataIndexCount %d \n", __FUNCTION__, __LINE__,
			        name, mDrmMetaDataIndexCount);
			for (int j = 0; j < mDrmMetaDataIndexCount; j++)
			{
				if (drmMetadataNode[j].sha1Hash)
				{
					printf("%s:%d drmMetadataNode[%d].sha1Hash -- \n", __FUNCTION__, __LINE__, j);
					for (int i = 0; i < DRM_SHA1_HASH_LEN; i++)
					{
						printf("%c", drmMetadataNode[j].sha1Hash[i]);
					}
					printf("\n");
				}
				else
				{
					logprintf("%s:%d drmMetadataNode[%d].sha1Hash NULL\n", __FUNCTION__, __LINE__, j);
				}
			}
			for (int i = 0; i < playlist.len; i++)
			{
				char temp = playlist.ptr[i];
				if (temp == '\0')
				{
					temp = '\n';
				}
				putchar(temp);
			}
			assert(false);
		}
	}
}
/***************************************************************************
* @fn UpdateDrmIV
* @brief Function to update IV from DRM 
*		 
* @param[in] ptr  IV string from DRM attribute
*
* @return void
***************************************************************************/
void TrackState::UpdateDrmIV(const char *ptr)
{
	size_t len;
	unsigned char *iv = base16_Decode(ptr, (DRM_IV_LEN*2), &len); // 32 characters encoding 128 bits (16 bytes)
	assert(len == DRM_IV_LEN);
	if(mDrmInfo.iv)
	{
		if(0 != memcmp(mDrmInfo.iv, iv, DRM_IV_LEN))
		{
			traceprintf("%s:%d Different DRM IV - ", __FUNCTION__, __LINE__);
#ifdef TRACE
			for (int i = 0; i< DRM_IV_LEN*2; i++)
			{
				printf("%c", ptr[i]);
			}
			printf("\n");
#endif
		}
		else
		{
			traceprintf("%s:%d Same DRM IV\n", __FUNCTION__, __LINE__);
		}
		free(mDrmInfo.iv);
	}
	mDrmInfo.iv = iv;
	traceprintf("%s:%d [%s] Exit mDrmInfo.iv %p\n", __FUNCTION__, __LINE__, name, mDrmInfo.iv);
}
/***************************************************************************
* @fn FetchPlaylist
* @brief Function to fetch playlist file
*		 
* @return void
***************************************************************************/

void TrackState::FetchPlaylist()
{
	int playlistDownloadFailCount = 0;
	long http_error;
	ProfilerBucketType bucketId = (this->type == eTRACK_AUDIO)?PROFILE_BUCKET_PLAYLIST_AUDIO:PROFILE_BUCKET_PLAYLIST_VIDEO;
	logprintf("TrackState::%s [%s] start\n", __FUNCTION__, name);
	aamp->profiler.ProfileBegin(bucketId);
	do
	{
		MediaType mType = (this->type == eTRACK_AUDIO)?eMEDIATYPE_PLAYLIST_AUDIO:eMEDIATYPE_PLAYLIST_VIDEO;
		aamp->GetFile(playlistUrl, &playlist, effectiveUrl, &http_error, NULL, type, true, mType);
		if(playlist.len)
		{
			aamp->profiler.ProfileEnd(bucketId);
			break;
		}
		logprintf("Playlist download failed : %s failure count : %d : http response : %d\n", playlistUrl, playlistDownloadFailCount, (int)http_error);
		aamp->InterruptableMsSleep(500);
		playlistDownloadFailCount += 1;
	} while(aamp->DownloadsAreEnabled() && (MAX_MANIFEST_DOWNLOAD_RETRY >  playlistDownloadFailCount) && (404 == http_error));
	logprintf("TrackState::%s [%s] end\n", __FUNCTION__, name);
	if (!playlist.len)
	{
		aamp->profiler.ProfileError(bucketId);
	}
}


/***************************************************************************
* @fn GetBWIndex
* @brief Function to get bandwidth index corresponding to bitrate
*
* @param bitrate Bitrate in bits per second
*
* @return bandwidth index
***************************************************************************/
int StreamAbstractionAAMP_HLS::GetBWIndex(long bitrate)
{
	int topBWIndex = 0;
	int profileCount = GetProfileCount();
	if (profileCount)
	{
		for (int i = 0; i < profileCount; i++)
		{
			struct HlsStreamInfo *streamInfo = &this->streamInfo[i];
			if (!streamInfo->isIframeTrack && streamInfo->bandwidthBitsPerSecond > bitrate)
			{
				--topBWIndex;
			}
		}
	}
	return topBWIndex;
}

/***************************************************************************
* @fn GetNextFragmentPeriodInfo
* @brief Function to get next playback position from start, to handle discontinuity 
*		 
* @param[out] periodIdx                  Period Index 	
* @param[out] offsetFromPeriodStart      Offset value
*
* @return void
***************************************************************************/
void TrackState::GetNextFragmentPeriodInfo(int &periodIdx, double &offsetFromPeriodStart)
{
	const IndexNode *index = (IndexNode *) this->index.ptr;
	const IndexNode *idxNode = NULL;
	periodIdx = -1;
	offsetFromPeriodStart = 0;
	int idx;
	double prevCompletionTimeSecondsFromStart = 0;
	assert(context->rate > 0);
	for (idx = 0; idx < indexCount; idx++)
	{
		const IndexNode *node = &index[idx];
		if (node->completionTimeSecondsFromStart >= playTarget)
		{
			logprintf("%s Found node - rate %f completionTimeSecondsFromStart %f playTarget %f\n", __FUNCTION__,
			        context->rate, node->completionTimeSecondsFromStart, playTarget);
			idxNode = node;
			break;
		}
		prevCompletionTimeSecondsFromStart = node->completionTimeSecondsFromStart;
	}
	if (idxNode)
	{
		if (idx > 0)
		{
			offsetFromPeriodStart = prevCompletionTimeSecondsFromStart;
			double periodStartPosition = 0;
			DiscontinuityIndexNode* discontinuityIndex = (DiscontinuityIndexNode*)mDiscontinuityIndex.ptr;
			for (int i = 0; i < mDiscontinuityIndexCount; i++)
			{
				traceprintf("TrackState::%s [%s] Loop periodItr %d idx %d first %d second %f\n", __FUNCTION__, name, i, idx,
				        discontinuityIndex[i].fragmentIdx, discontinuityIndex[i].position);
				if (discontinuityIndex[i].fragmentIdx > idx)
				{
					logprintf("TrackState::%s [%s] Found periodItr %d idx %d first %d offsetFromPeriodStart %f\n",
					        __FUNCTION__, name, i, idx, discontinuityIndex[i].fragmentIdx, periodStartPosition);
					break;
				}
				periodIdx = i;
				periodStartPosition = discontinuityIndex[i].position;
			}
			offsetFromPeriodStart -= periodStartPosition;
		}
		logprintf("TrackState::%s [%s] periodIdx %d offsetFromPeriodStart %f\n", __FUNCTION__, name, periodIdx,
		        offsetFromPeriodStart);
	}
	else
	{
		logprintf("TrackState::%s [%s] idxNode NULL\n", __FUNCTION__, name);
	}
}
/***************************************************************************
* @fn GetPeriodStartPosition
* @brief Function to get Period start position for given period index,to handle discontinuity
*		 
* @param[in] periodIdx  Period Index 
*
* @return void
***************************************************************************/
double TrackState::GetPeriodStartPosition(int periodIdx)
{
	double offset = 0;
	logprintf("TrackState::%s [%s] periodIdx %d periodCount %d\n", __FUNCTION__, name, periodIdx,
	        (int) mDiscontinuityIndexCount);
	if (periodIdx < mDiscontinuityIndexCount)
	{
		int count = 0;
		DiscontinuityIndexNode* discontinuityIndex = (DiscontinuityIndexNode*)mDiscontinuityIndex.ptr;
		for (int i = 0; i < mDiscontinuityIndexCount; i++)
		{
			if (count == periodIdx)
			{
				offset = discontinuityIndex[i].position;
				logprintf("TrackState::%s [%s] offset %f periodCount %d\n", __FUNCTION__, name, offset,
				        (int) mDiscontinuityIndexCount);
				break;
			}
			else
			{
				count++;
			}
		}
	}
	else
	{
		logprintf("TrackState::%s [%s] WARNING periodIdx %d periodCount %d\n", __FUNCTION__, name, periodIdx,
		        mDiscontinuityIndexCount);
	}
	return offset;
}
/***************************************************************************
* @fn GetNumberOfPeriods
* @brief Function to return number of periods stored in playlist
*		 
* @return int number of periods
***************************************************************************/
int TrackState::GetNumberOfPeriods()
{
	return mDiscontinuityIndexCount;
}

/***************************************************************************
* @fn HasDiscontinuityAroundPosition
* @brief Check if discontinuity present around given position
* @param[in] position Position to check for discontinuity
* @param[out] diffBetweenDiscontinuities discontinuity position minus input position
* @return true if discontinuity present around given position
***************************************************************************/
bool TrackState::HasDiscontinuityAroundPosition(double position, bool useStartTime, double &diffBetweenDiscontinuities, double playPosition)
{
	bool discontinuityPending = false;
	double low = position - DISCONTINUITY_DISCARD_TOLERANCE_SECONDS;
	double high = position + DISCONTINUITY_DISCARD_TOLERANCE_SECONDS;
	int playlistRefreshCount = 0;
	diffBetweenDiscontinuities = DBL_MAX;
	pthread_mutex_lock(&mPlaylistMutex);
	while (aamp->DownloadsAreEnabled())
	{
		if (0 != mDiscontinuityIndexCount)
		{
			DiscontinuityIndexNode* discontinuityIndex = (DiscontinuityIndexNode*)mDiscontinuityIndex.ptr;
			for (int i = 0; i < mDiscontinuityIndexCount; i++)
			{
				if ((mLastMatchedDiscontPosition < 0)
						|| (discontinuityIndex[i].position + mCulledSeconds > mLastMatchedDiscontPosition))
				{
					if (!useStartTime)
					{
						traceprintf ("%s:%d low %f high %f position %f discontinuity %f\n", __FUNCTION__, __LINE__, low, high, position, discontinuityIndex[i].position);
						if (low < discontinuityIndex[i].position && high > discontinuityIndex[i].position)
						{
							mLastMatchedDiscontPosition = discontinuityIndex[i].position + mCulledSeconds;
							discontinuityPending = true;
							break;
						}
					}
					else
					{
						struct timeval programDateTimeVal;
						if (ParseTimeFromProgramDateTime(discontinuityIndex[i].programDateTime, programDateTimeVal))
						{
							double discPos = programDateTimeVal.tv_sec + (double) programDateTimeVal.tv_usec/1000000;
							logprintf ("%s:%d low %f high %f position %f discontinuity %f\n", __FUNCTION__, __LINE__, low, high, position, discPos);

							if (low < discPos && high > discPos)
							{
								double diff = discPos - position;
								discontinuityPending = true;
								if (fabs(diff) < fabs(diffBetweenDiscontinuities))
								{
									diffBetweenDiscontinuities = diff;
									mLastMatchedDiscontPosition = discontinuityIndex[i].position + mCulledSeconds;
								}
								else
								{
									break;
								}
							}
						}
					}
				}
			}
		}
		if (!discontinuityPending)
		{
			logprintf("%s:%d ##[%s] Discontinuity not found in window low %f high %f position %f mLastMatchedDiscontPosition %f mDuration %f playPosition %f playlistRefreshCount %d playlistType %d useStartTime %d\n", __FUNCTION__, __LINE__,
					name, low, high, position, mLastMatchedDiscontPosition, mDuration, playPosition, playlistRefreshCount, (int)context->playlistType, (int)useStartTime);
			if (ePLAYLISTTYPE_VOD != context->playlistType)
			{
				int maxPlaylistRefreshCount;
				bool liveNoTSB;
				if (aamp->IsTSBSupported() || aamp->IsInProgressCDVR())
				{
					maxPlaylistRefreshCount = MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_EVENT;
					liveNoTSB = false;
				}
				else
				{
					maxPlaylistRefreshCount = MAX_PLAYLIST_REFRESH_FOR_DISCONTINUITY_CHECK_LIVE;
					liveNoTSB = true;
				}
				if ((playlistRefreshCount < maxPlaylistRefreshCount)
						&& (liveNoTSB || (mDuration < (playPosition + DISCONTINUITY_DISCARD_TOLERANCE_SECONDS))))
				{
					logprintf("%s:%d Waiting for %s playlist update mDuration %f mCulledSeconds %f\n", __FUNCTION__,
					        __LINE__, name, mDuration, mCulledSeconds);
					pthread_cond_wait(&mPlaylistIndexed, &mPlaylistMutex);
					logprintf("%s:%d Wait for %s playlist update over\n", __FUNCTION__, __LINE__, name);
					playlistRefreshCount++;
				}
				else
				{
					break;
				}
			}
			else
			{
				break;
			}
		}
		else
		{
			break;
		}
	}
	pthread_mutex_unlock(&mPlaylistMutex);
	return discontinuityPending;
}

/***************************************************************************
* @brief Fetch init fragment for fragmented mp4 format
* @return true if success
***************************************************************************/
bool TrackState::FetchInitFragment(long &http_code)
{
	bool ret = false;
	traceprintf("%s:%d Enter\n", __FUNCTION__, __LINE__);
	std::istringstream initFragmentUrlStream(mInitFragmentInfo);
	std::string line;
	std::getline(initFragmentUrlStream, line);
	if (!line.empty())
	{
		const char *range = NULL;
		char rangeStr[128];
		std::string uri;
		traceprintf("%s:%d line %s\n", __FUNCTION__, __LINE__, line.c_str());
		size_t uriTagStart = line.find("URI=");
		if (uriTagStart != std::string::npos)
		{
			std::string uriStart = line.substr(uriTagStart + 5);
			traceprintf("%s:%d uriStart %s\n", __FUNCTION__, __LINE__, uriStart.c_str());
			size_t uriTagEnd = uriStart.find("\"");
			if (uriTagEnd != std::string::npos)
			{
				traceprintf("%s:%d uriTagEnd %d\n", __FUNCTION__, __LINE__, (int) uriTagEnd);
				uri = uriStart.substr(0, uriTagEnd);
				traceprintf("%s:%d uri %s\n", __FUNCTION__, __LINE__, uri.c_str());
			}
			else
			{
				logprintf("%s:%d URI parse error. Tag end not found \n", __FUNCTION__, __LINE__);
			}
		}
		else
		{
			logprintf("%s:%d URI parse error. URI= not found\n", __FUNCTION__, __LINE__);
		}
		size_t byteRangeTagStart = line.find("BYTERANGE=");
		if (byteRangeTagStart != std::string::npos)
		{
			std::string byteRangeStart = line.substr(byteRangeTagStart + 11);
			size_t byteRangeTagEnd = byteRangeStart.find("\"");
			if (byteRangeTagEnd != std::string::npos)
			{
				std::string byteRange = byteRangeStart.substr(0, byteRangeTagEnd);
				traceprintf("%s:%d byteRange %s\n", __FUNCTION__, __LINE__, byteRange.c_str());
				if (!byteRange.empty())
				{
					size_t offsetIdx = byteRange.find("@");
					if (offsetIdx != std::string::npos)
					{
						int offsetVal = stoi(byteRange.substr(offsetIdx + 1));
						int rangeVal = stoi(byteRange.substr(0, offsetIdx));
						int next = offsetVal + rangeVal;
						sprintf(rangeStr, "%d-%d", offsetVal, next - 1);
						logprintf("%s:%d rangeStr %s \n", __FUNCTION__, __LINE__, rangeStr);
						range = rangeStr;
					}
				}
			}
			else
			{
				logprintf("%s:%d byteRange parse error. Tag end not found byteRangeStart %s\n",
						__FUNCTION__, __LINE__, byteRangeStart.c_str());
			}
		}
		if (!uri.empty())
		{
			char fragmentUrl[MAX_URI_LENGTH];
			aamp_ResolveURL(fragmentUrl, effectiveUrl, uri.c_str());
			char tempEffectiveUrl[MAX_URI_LENGTH];
			WaitForFreeFragmentAvailable();
			CachedFragment* cachedFragment = GetFetchBuffer(true);
			logprintf("%s:%d fragmentUrl = %s \n", __FUNCTION__, __LINE__, fragmentUrl);
			bool fetched = aamp->GetFile(fragmentUrl, &cachedFragment->fragment, tempEffectiveUrl, &http_code, range,
			        type, false, (MediaType) (type));
			if (!fetched)
			{
				logprintf("%s:%d aamp_GetFile failed\n", __FUNCTION__, __LINE__);
				aamp_Free(&cachedFragment->fragment.ptr);
			}
			else
			{
				UpdateTSAfterFetch();
				ret = true;
			}
		}
		else
		{
			logprintf("%s:%d Could not parse URI. line %s\n", __FUNCTION__, __LINE__, line.c_str());
		}
	}
	else
	{
		logprintf("%s:%d Parse error\n", __FUNCTION__, __LINE__);
	}
	return ret;
}

/***************************************************************************
* @fn StopInjection
* @brief Function to stop fragment injection
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::StopInjection(void)
{
	//invoked at times of discontinuity. Audio injection loop might have already exited here
	ReassessAndResumeAudioTrack(true);

	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track && track->Enabled())
		{
			track->StopInjection();
		}
	}
}

/***************************************************************************
* @fn StopInjection
* @brief Function to stop fragment injection
*
* @return void
***************************************************************************/
void TrackState::StopInjection()
{
	AbortWaitForCachedFragment(true);
	if (playContext)
	{
		playContext->abort();
	}
	StopInjectLoop();
}

/***************************************************************************
* @fn StartInjection
* @brief starts fragment injection
*
* @return void
***************************************************************************/
void TrackState::StartInjection()
{
	if (playContext)
	{
		playContext->reset();
	}
	StartInjectLoop();
}

/***************************************************************************
* @fn StartInjection
* @brief Function to start fragment injection
*
* @return void
***************************************************************************/
void StreamAbstractionAAMP_HLS::StartInjection(void)
{
	abortWait = false;
	for (int iTrack = 0; iTrack < AAMP_TRACK_COUNT; iTrack++)
	{
		TrackState *track = trackState[iTrack];
		if(track && track->Enabled())
		{
			track->StartInjection();
		}
	}
}

/***************************************************************************
* @fn StopWaitForPlaylistRefresh
* @brief Stop wait for playlist refresh
*
* @return void
***************************************************************************/
void TrackState::StopWaitForPlaylistRefresh()
{
	logprintf("%s:%d track [%s]\n", __FUNCTION__, __LINE__, name);
	pthread_mutex_lock(&mPlaylistMutex);
	pthread_cond_signal(&mPlaylistIndexed);
	pthread_mutex_unlock(&mPlaylistMutex);
}

/**
 * @}
 */
