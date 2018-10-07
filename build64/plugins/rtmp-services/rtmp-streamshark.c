#include <obs-module.h>
#include <stdio.h>
#include <curl.h>
#include <string.h>
#include <stdlib.h>

#define CHANNEL_BUFFER 100
#define STR_BUFFER 100

// example URL call
// http://www.metacdn.com/wirecast/channelDiscovery?username=<USERNAME>&password=<PASS>
struct rtmp_streamshark {
	char *server, *key;
	bool use_auth;
	char *username, *password;
};

typedef struct rtmp_streamshark_channel_struct {
	char *server, *key;
	char *username, *password;
	bool title;
} *rtmp_streamshark_channel;

struct url_data{
	size_t size;
	char* data;
};

rtmp_streamshark_channel streamshark_channels[CHANNEL_BUFFER];
char* enc_username;
char* enc_password;

static const char *rtmp_streamshark_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "StreamShark";
}

static void rtmp_streamshark_update(void *data, obs_data_t *settings)
{
	struct rtmp_streamshark *service = data;

	bfree(service->server);
	bfree(service->key);

	int channeListIndex = obs_data_get_int(settings, "channelList");
	if (channeListIndex == -1 || streamshark_channels[channeListIndex] == NULL)
		return;

	rtmp_streamshark_channel selectedChannel = streamshark_channels[channeListIndex];
	
	service->server = selectedChannel->server;
	service->key    = selectedChannel->key;
	service->use_auth = true;
	service->username = selectedChannel->username;
	service->password = selectedChannel->password;
}

static void rtmp_streamshark_destroy(void *data)
{
	struct rtmp_streamshark *service = data;

	bfree(service->server);
	bfree(service->key);
	bfree(service->username);
	bfree(service->password);
	bfree(service);
}

static void *rtmp_streamshark_create(obs_data_t *settings, obs_service_t *service)
{
	struct rtmp_streamshark *data = bzalloc(sizeof(struct rtmp_streamshark));
	rtmp_streamshark_update(data, settings);

	UNUSED_PARAMETER(service);
	return data;
}

static bool use_auth_modified(obs_properties_t *ppts, obs_property_t *p,
	obs_data_t *settings)
{
	bool use_auth = obs_data_get_bool(settings, "use_auth");
	p = obs_properties_get(ppts, "username");
	obs_property_set_visible(p, use_auth);
	p = obs_properties_get(ppts, "password");
	obs_property_set_visible(p, use_auth);
	return true;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, struct url_data *data) {
	size_t index = data->size;
	size_t n = (size * nmemb);
	char* tmp;

	data->size += (size * nmemb);

#ifdef DEBUG
	fprintf(stderr, "data at %p size=%ld nmemb=%ld\n", ptr, size, nmemb);
#endif
	tmp = realloc(data->data, data->size + 1); /* +1 for '\0' */

	if (tmp) {
		data->data = tmp;
	}
	else {
		if (data->data) {
			free(data->data);
		}
		fprintf(stderr, "Failed to allocate memory.\n");
		return 0;
	}

	memcpy((data->data + index), ptr, n);
	data->data[data->size] = '\0';

	return size * nmemb;
}

char *handle_url(char* url) {
	CURL *curl;

	struct url_data data;
	data.size = 0;
	data.data = malloc(4096); /* reasonable size initial buffer */
	if (NULL == data.data) {
		fprintf(stderr, "Failed to allocate memory.\n");
		return NULL;
	}

	data.data[0] = '\0';

	CURLcode res;

	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
		res = curl_easy_perform(curl);
		if (res != CURLE_OK) {
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
		}

		curl_easy_cleanup(curl);

	}
	return data.data;
}

size_t write_clbk(void *data, size_t blksz, size_t nblk, void *ctx)
{
	static size_t sz = 0;
	size_t currsz = blksz * nblk;

	size_t prevsz = sz;
	sz += currsz;
	void *tmp = realloc(*(char **)ctx, sz);
	if (tmp == NULL) {
		// handle error
		free(*(char **)ctx);
		*(char **)ctx = NULL;
		return 0;
	}
	*(char **)ctx = tmp;

	memcpy(*(char **)ctx + prevsz, data, currsz);
	return currsz;
}

static bool check_response_code(char* str) {
	// too lazy to do this properly
	char *result = strstr(str, "<error code");
	if (result == NULL)
		return false;
	int position = result - str + 13;
	return atoi(str + position) == 0;
}

static void clear_channels(obs_properties_t *props) {
	obs_property_t* p = obs_properties_get(props, "channelList");

	obs_property_list_clear(p);

	for (int i = 0; i < CHANNEL_BUFFER; i++) {
		streamshark_channels[i] = NULL;
	}
}

// unsafe
static char* retreive_substring(char* source, char* beforeStr, char* afterStr) {
	int strBeforeLen = strlen(beforeStr);

	char* startIndex = strstr(source, beforeStr);
	if (startIndex == NULL)
		return NULL;
	startIndex += strBeforeLen;
	int startIndexPosition = startIndex - source;

	char* endIndex = strstr(startIndex, afterStr);
	if (endIndex == NULL)
		return NULL;
	int endIndexPosition = endIndex - source;
	int newStrLen = endIndexPosition - startIndexPosition + 1;

	char* substr = malloc(newStrLen);
	strncpy(substr, startIndex, newStrLen - 1);	
	substr[newStrLen - 1] = '\0';

	return substr;
}

static void add_channels_to_list(obs_properties_t *props, char* source) {
	clear_channels(props);
	obs_property_t* p = obs_properties_get(props, "channelList");

	// todo a better way to do this
	char* next;
	int i = -1, strptr = 0;

	while ((next = strstr(source + strptr, "<channel ")) != NULL) {
		// loop management
		i++;
		char* sourceStart = source + strptr;
		// find the end of this channel
		char* endChannel = strstr(sourceStart, "</channel>");
		int endChannelPosition = endChannel - (sourceStart) + strlen("</channel>");
		
		rtmp_streamshark_channel entry = (rtmp_streamshark_channel)malloc(sizeof(*entry));
		streamshark_channels[i] = entry;

		entry->server = retreive_substring(sourceStart, "rtmp=\"", "\" stream=\"");
		entry->key = retreive_substring(sourceStart, "\" stream=\"", "\" text=\"");
		
		char* text = retreive_substring(sourceStart, "\" text=\"", "\" language=");

		// check if this is a dummy line or not
		if (strstr(text, "=\"") == NULL) {
			// this is a dummy channel
			entry->username = NULL;
			entry->password = NULL;
			entry->title = true;
		}
		else {
			// this is real
			text = retreive_substring(sourceStart, "\" text=\"", "\" type=");
			entry->username = retreive_substring(sourceStart, "\" username=\"", "\" password=\"");
			entry->password = retreive_substring(sourceStart, "\" password=\"", "\" language=\"");
			entry->title = false;
		}

		obs_property_list_add_int(p, text, i);
				
		strptr += endChannelPosition;
	}
}

static bool are_credentials_valid(char* username, char* password) {
	// not empty for now
	return username != NULL && strlen(username) != 0 && password != NULL && strlen(password) != 0;
}

static bool login_button_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	// validate
	if (!are_credentials_valid(enc_username, enc_password))
		return true;
	
	char* serverResult;
	char* url = malloc(sizeof(char) * STR_BUFFER * 4);

	strcpy(url, "http://www.metacdn.com/wirecast/channelDiscovery?username=");
	strcat(url, enc_username);
	strcat(url, "&password=");
	strcat(url, enc_password);

	serverResult = handle_url(url);

	if (!serverResult)  return true;
	
	printf("%s\n", serverResult);
	bool pass = check_response_code(serverResult);
	if (!pass) {
		return true;
		free(serverResult);
	}

	add_channels_to_list(props, serverResult);
	free(serverResult);
	
	return true;
}

static bool channel_list_updated(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	int samplerate = obs_data_get_int(settings, "channelList");
	return true;
}

static bool username_callback(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings) {

	enc_username = obs_data_get_string(settings, "username");
	return false;
}
static bool password_callback(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings) {

	enc_password = obs_data_get_string(settings, "password");
	return false;
}

static obs_properties_t *rtmp_streamshark_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_text(ppts, "username", "Username", OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback(p, username_callback);
	p = obs_properties_add_text(ppts, "password", "Encoder Password", OBS_TEXT_PASSWORD);
	obs_property_set_modified_callback(p, password_callback);

	obs_properties_add_button(ppts, "login", "Login", login_button_clicked);

	p = obs_properties_add_list(ppts, "channelList", "Channels", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(p, channel_list_updated);
	obs_property_list_add_int(p, "Login Pls", -1);

	return ppts;
}

static const char *rtmp_streamshark_url(void *data)
{
	struct rtmp_streamshark *service = data;
	return service->server;
}

static const char *rtmp_streamshark_key(void *data)
{
	struct rtmp_streamshark *service = data;
	return service->key;
}

static const char *rtmp_streamshark_username(void *data)
{
	struct rtmp_streamshark *service = data;
	if (!service->use_auth)
		return NULL;
	return service->username;
}

static const char *rtmp_streamshark_password(void *data)
{
	struct rtmp_streamshark *service = data;
	if (!service->use_auth)
		return NULL;
	return service->password;
}

struct obs_service_info rtmp_streamshark_service = {
	.id             = "rtmp_streamshark",
	.get_name       = rtmp_streamshark_name,
	.create         = rtmp_streamshark_create,
	.destroy        = rtmp_streamshark_destroy,
	.update         = rtmp_streamshark_update,
	.get_properties = rtmp_streamshark_properties,
	.get_url        = rtmp_streamshark_url,
	.get_key        = rtmp_streamshark_key,
	.get_username   = rtmp_streamshark_username,
	.get_password   = rtmp_streamshark_password
};
