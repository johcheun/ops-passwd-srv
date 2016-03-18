/*
 * (c) Copyright 2016 Hewlett Packard Enterprise Development LP
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <crypt.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#include "passwd_srv_pri.h"

/*
 *  Generate salt of size salt_size.
 */
#define MAX_SALT_SIZE 16
#define MIN_SALT_SIZE 8

#define MAGNUM(array,ch) (array)[0]=(array)[2]='$',(array)[1]=(ch),(array)[3]='\0'

static char *crypt_method = NULL;

/*
 * RNG function to generate seed to make salt
 *
 * @param reset whether API needs to re-seed
 */
static
void create_seed (int reset)
{
    struct timeval time_value;
    static int seeded = 0;

    seeded = (reset) ? 0 : seeded;

    if (!seeded)
    {
        gettimeofday (&time_value, NULL);
        srandom (time_value.tv_sec ^ time_value.tv_usec ^ getgid ());
        seeded = 1;
    }
}

/*
 * make salt based on size provided by caller
 *
 * @param salt_size size of salt
 * @return salt generated, or NULL if error happens
 */
static
const char *generate_salt (size_t salt_size)
{
    static char salt[32];

    salt[0] = '\0';

    if(! (salt_size >= MIN_SALT_SIZE &&
            salt_size <= MAX_SALT_SIZE))
    {
        return NULL;
    }
    create_seed (0);
    strcat (salt, l64a (random()));
    do {
        strcat (salt, l64a (random()));
    } while (strlen (salt) < salt_size);

    salt[salt_size] = '\0';

    return salt;
}

/*
 * Return the salt size.
 * The size of the salt string is between 8 and 16 bytes for the SHA crypt
 * methods.
 */
static size_t SHA_salt_size ()
{
    double rand_size;
    create_seed (0);
    rand_size = (double) 9.0 * random () / RAND_MAX;
    return (size_t) (8 + rand_size);
}

/**
 * Search thru login.defs file and return value string that found.
 *
 * @param target string to search in login.defs
 * @return value found from searching string, NULL if target string is not
 *          found
 */
static
char *search_login_defs(const char *target)
{
    char line[1024], *value;
    FILE *fpLogin;

    /* find encrypt_method and assign it to static crypt_method */
    if (NULL == (fpLogin = fopen(PASSWD_LOGIN_FILE, "r")))
    {
        /* cannot open login.defs file for read */
        return NULL;
    }

    while (fgets(line, sizeof(line), fpLogin))
    {
        if ((0 == memcmp(line, target, strlen(target))) &&
            (' ' == line[strlen(target)]))
        {
            /* found matching string, find next token and return */
            char *temp = &(line[strlen(target) + 1]);
            value = strdup(temp);
            value[strlen(value)] = '\0';

            return value;
        }
    } /* while */

    fclose(fpLogin);
    return NULL;
}

/**
 * Create a user using useradd program
 *
 * @param username username to add
 * @param useradd  add if true, deleate otherwise
 */
static
struct spwd *create_user(const char *username, int useradd)
{
    char useradd_comm[512];
    struct spwd *passwd_entry = NULL;

    memset(useradd_comm, 0, sizeof(useradd_comm));

    if (useradd)
    {
        snprintf(useradd_comm, sizeof(useradd_comm),
            "%s -g %s -G %s -s %s %s", USERADD, NETOP_GROUP, OVSDB_GROUP,
            VTYSH_PROMPT, username);
    }
    else
    {
        snprintf(useradd_comm, sizeof(useradd_comm),
                    "%s %s", USERDEL, username);
    }

    if (0 > system(useradd_comm))
    {
        return NULL;
    }

    /* make sure that user has been created */
    if (useradd && NULL == (passwd_entry = find_password_info(username)))
    {
        return NULL;
    }

    return passwd_entry;
}

/**
 * Look into login.defs file to find encryption method
 *  If encrypt_method is not found, hashing algorighm
 *  falls back to MD5 or DES.
 */
static
void find_encrypt_method()
{
    char *method = NULL;

    /* search login.defs to get method */
    method = search_login_defs("ENCRYPT_METHOD");

    if (NULL == method)
    {
        /* couldn't find encrypt_method, search for md5 */
        method = search_login_defs("MD5_CRYPT_ENAB");

        if (NULL == method || 0 == strncmp(method, "no", strlen(method)))
        {
            crypt_method = strdup("DES");
        }
        else
        {
            crypt_method = strdup("MD5");
        }

        return;
    }

    crypt_method = strdup(method);

    free(method);
}

/**
 * Create new salt to be used to create hashed password
 */
static
char *create_new_salt()
{
    /* Max result size for the SHA methods:
     *  +3      $5$
     *  +17     rounds=999999999$
     *  +16     salt
     *  +1      \0
     */
    static char result[40];
    size_t salt_len = 8;

    /* notify seed RNG to reset its seeded value to seeding again */
    create_seed(1);

    /* TODO: find a way to handle login.defs file change */
    if (NULL == crypt_method)
    {
        /* find out which method to use */
        find_encrypt_method();
    }

    if (0 == strncmp (crypt_method, "MD5", strlen("MD5")))
    {
        MAGNUM(result, '1');
    }
    else if (0 == strncmp (crypt_method, "SHA256", strlen("SHA256")))
    {
        MAGNUM(result, '5');
        salt_len = SHA_salt_size();
    }
    else if (0 == strncmp (crypt_method, "SHA512", strlen("SHA512")))
    {
        MAGNUM(result, '6');
        salt_len = SHA_salt_size();
    }
    else if (0 != strncmp (crypt_method, "DES", strlen("DES")))
    {
        result[0] = '\0';
    }
    else
    {
        return NULL;
    }

    /*
     * Concatenate a pseudo random salt.
     */
    strncat (result, generate_salt (salt_len),
         sizeof (result) - strlen (result) - 1);

    return strdup(result);
}

/*
 * Update password for the user. Search for the username in /etc/shadow and
 * update password string with on passed onto it.
 *
 * @param user username to find
 * @param pass password to store
 * @return SUCCESS if updated, error code if fails to update
 */
int store_password(char *user, char *pass)
{
    FILE *fpShadow;
    long int cur_pos = 0;
    struct spwd *cur_user;
    int cur_uname_len, uname_len;
    char newpass[512];
    int err = PASSWD_ERR_PASSWD_UPD_FAIL;

    memset(newpass, 0, sizeof(newpass));
    memcpy(newpass, pass, strlen(pass));

    uname_len = strlen(user);

    /* lock shadow file */
    if (0 != lckpwdf())
    {
        return PASSWD_ERR_FATAL;
    }

    if (NULL == (fpShadow = fopen(PASSWD_SHADOW_FILE, "r+a")))
    {
        return PASSWD_ERR_FATAL;
    }

    /* save file position */
    cur_pos = ftell(fpShadow);

    while((cur_user = fgetspent(fpShadow)))
    {
        cur_uname_len = strlen(cur_user->sp_namp);

       if ( (cur_uname_len == uname_len) &&
               (0 == strncmp(cur_user->sp_namp, user, strlen(user))) )
       {
           /* found the match, set file pointer to current user location */
           fsetpos(fpShadow, (const fpos_t*)&cur_pos);

           cur_user->sp_pwdp = newpass;

           /* update password info */
           putspent(cur_user, fpShadow);

           err = PASSWD_ERR_SUCCESS;
           break;
       }

       /* save file position */
       cur_pos = ftell(fpShadow);
    }

    /* unlock shadow file */
    ulckpwdf();
    fclose(fpShadow);

    return err;
}

/*
 * Create salt/password to update password in /etc/shadow
 *
 * @param client target client to update password
 * @return SUCCESS if password updated, error code otherwise
 */
int create_and_store_password(passwd_client_t *client)
{
    char *salt = NULL;
    char *password, *newpassword;
    int  err = 0;

    if ((NULL == client) || (NULL == client->passwd))
    {
        return PASSWD_ERR_INVALID_PARAM;
    }

    salt = create_new_salt();
    password = strdup(client->msg.newpasswd);

    /* generate new password using crypt */
    newpassword = crypt(password, salt);

    /* store it to shadow file */
    err = store_password(client->msg.username, newpassword);

    memset(newpassword, 0, strlen(newpassword));
    memset(salt, 0, strlen(salt));
    free(salt);

    return err;
}

/**
 * validate user information using socket descriptor and passwd file
 *
 * @param sockaddr  sockaddr structure for client connection
 * @param client    client structure entry
 *
 * @return 0 if client is ok to update pasword
 */
int validate_user(struct sockaddr_un *sockaddr, passwd_client_t *client)
{
    struct stat     c_stat;
    struct passwd   *user = NULL;

    if (NULL == client)
    {
        return PASSWD_ERR_INVALID_USER;
    }

    memset(&c_stat, 0, sizeof(c_stat));

    /* call stat() to get user information */
    stat((const char*)client->msg.file_path, &c_stat);

    if (NULL == (user = getpwuid(c_stat.st_uid)))
    {
        return PASSWD_ERR_INVALID_USER;
    }

    /* user is found, compare with client info */
    if (0 == strncmp(user->pw_name, client->msg.username,
            strlen(client->msg.username)))
    {
        /* sender is user who wants to change own password */
        return 0;
    }
    else if (0 == strncmp(user->pw_name, "ops", strlen("ops")))
    {
        /* sender is ops who wants to change user password */
        return 0;
    }

    return PASSWD_ERR_INVALID_USER;
}

/**
 * validate password by using crypt function
 *
 * @param client
 * @return 0 if passwords are matched
 */
int validate_password(passwd_client_t *client)
{
    char *crypt_str = NULL;
    int  err = 0;

    if ((NULL == (crypt_str = crypt(client->msg.oldpasswd,
            client->passwd->sp_pwdp))) ||
        (0 != strncmp(crypt_str, client->passwd->sp_pwdp,
                strlen(client->passwd->sp_pwdp))))
    {
        err = PASSWD_ERR_FATAL;
    }

    if (NULL != crypt_str)
    {
        memset(crypt_str, 0, strlen(crypt_str));
    }
    return err;
}

/**
 * Find password info for a given user in /etc/shadow file
 *
 * @param  username[in] username to search
 * @return password     parsed shadow entry
 */
struct spwd *find_password_info(const char *username)
{
    struct spwd *password = NULL;
    FILE *fpShadow;
    int uname_len, cur_uname_len, name_len;

    if (NULL == username)
    {
        return NULL;
    }

    /* lock /etc/shadow file to read */
    if (0 != lckpwdf())
    {
        /* TODO: logging for failure */
        return NULL;
    }

    /* open shadow file */
    if (NULL == (fpShadow = fopen(PASSWD_SHADOW_FILE, "r")))
    {
        /* TODO: logging for failure */
        return NULL;
    }

    uname_len = strlen(username);

    /* loop thru /etc/shadow to find user */
    while(NULL != (password = fgetspent(fpShadow)))
    {
        cur_uname_len = strlen(password->sp_namp);
        name_len = (cur_uname_len >= uname_len) ? cur_uname_len : uname_len;

        if (0 == memcmp(password->sp_namp, username, name_len))
        {
            /* unlock shadow file */
            if (0 != ulckpwdf())
            {
                /* TODO: logging for failure */
            }
            fclose(fpShadow);
            return password;
        }
    }

    /* unlock shadow file */
    if (0 != ulckpwdf())
    {
        /* TODO: logging for failure */
    }

    fclose(fpShadow);
    return NULL;
}

/**
 * Process received MSG from client.
 *
 * @param client received MSG from client
 * @return if processed it successfully, return 0
 */
int process_client_request(passwd_client_t *client)
{
    int error = PASSWD_ERR_FATAL;

    if (NULL == client)
    {
        return -1;
    }

    switch(client->msg.op_code)
    {
    case PASSWD_MSG_CHG_PASSWORD:
    {
        /* proceed to change password for the user */
        if (NULL == (client->passwd = find_password_info(client->msg.username)))
        {
            /* logging error */
            return PASSWD_ERR_USER_NOT_FOUND;
        }

        /* validate old password */
        if (0 != validate_password(client))
        {
            return PASSWD_ERR_PASSWORD_NOT_MATCH;
        }

        if (PASSWD_ERR_SUCCESS == (error = create_and_store_password(client)))
        {
            printf("Password updated successfully for user\n");
        }
        else
        {
            printf("Password was not updated successfully [error=%d]\n", error);
        }
        break;
    }
    case PASSWD_MSG_ADD_USER:
    {
        /* make sure username does not exist */
        if (NULL != (client->passwd = find_password_info(client->msg.username)))
        {
            /* TODO: logging error */
            return PASSWD_ERR_USER_EXIST;
        }

        /* add user to /etc/passwd file */
        if (NULL == (client->passwd = create_user(client->msg.username, TRUE)))
        {
            /* failed to create user or getting information from /etc/passwd */
            return PASSWD_ERR_USERADD_FAILED;
        }

        /* now add password for the user */
        if (PASSWD_ERR_SUCCESS == (error = create_and_store_password(client)))
        {
            printf("User was added successfully\n");
        }
        else
        {
            printf("User was not added successfully [error=%d]\n", error);
            /* delete user since it failed to add password */
            create_user(client->msg.username, FALSE);
        }
        break;
    }
    case PASSWD_MSG_DEL_USER:
    {
        /* make sure username does not exist */
        if (NULL == (client->passwd = find_password_info(client->msg.username)))
        {
            /* TODO: logging error */
            return PASSWD_ERR_USER_NOT_FOUND;
        }

        /* delete user from /etc/passwd file */
        if (NULL != (client->passwd = create_user(client->msg.username, FALSE)))
        {
            /* failed to create user or getting information from /etc/passwd */
            return PASSWD_ERR_USERDEL_FAILED;
        }

        error = PASSWD_ERR_SUCCESS;
        break;
    }
    default:
    {
        /* wrong op-code */
        return PASSWD_ERR_INVALID_OPCODE;
    }
    }
    return error;
}

/**
 * Create ini file to expose variables defined in public header
 */
int create_ini_file()
{
    FILE *fp = NULL;

    if (NULL == (fp = fopen(PASSWD_SRV_INI_FILE, "w")))
    {
        /* TODO: logging */
        return PASSWD_ERR_FATAL;
    }

    /* write public key file location */
    fputs("# public key location\n", fp);
    fputs("[pub_key_loc_type]\n", fp);
    fputs("PASSWD_SRV_PUB_KEY_LOC_TYPE=string\n", fp);
    fputs("\n", fp);
    fputs("[pub_key_loc]\n", fp);
    fprintf(fp, "PASSWD_SRV_PUB_KEY_LOC=%s\n", PASSWD_SRV_PUB_KEY_LOC);
    fputs("\n", fp);

    /* write socket descriptor location */
    fputs("# server socket descriptor\n", fp);
    fputs("[socket_fd_type]\n", fp);
    fputs("PASSWD_SRV_SOCK_FD_TYPE=string\n", fp);
    fputs("\n", fp);
    fputs("[socket_fd_loc]\n", fp);
    fprintf(fp, "PASSWD_SRV_SOCK_FD=%s\n", PASSWD_SRV_SOCK_FD);
    fputs("\n", fp);

    /* write opcode */
    fputs("# message op code\n", fp);
    fputs("[op_code_size]\n", fp);
    fprintf(fp, "PASSWD_MSG_SIZE=%d\n", (int)sizeof(int));
    fputs("\n", fp);
    fputs("[op_code]\n", fp);
    fprintf(fp, "PASSWD_MSG_CHG_PASSWORD=%d\n", PASSWD_MSG_CHG_PASSWORD);
    fprintf(fp, "PASSWD_MSG_ADD_USER=%d\n", PASSWD_MSG_ADD_USER);
    fputs("\n", fp);

    /* write error codes */
    fputs("# error code used by password server\n", fp);
    fputs("[error_code_size]\n", fp);
    fprintf(fp, "PASSWD_ERR_CODE_SIZE=%d\n", (int)sizeof(int));
    fputs("[error_code]\n", fp);
    fprintf(fp, "PASSWD_ERR_FATAL=%d\n", PASSWD_ERR_FATAL);
    fprintf(fp, "PASSWD_ERR_SUCCESS=%d\n", PASSWD_ERR_SUCCESS);
    fprintf(fp, "PASSWD_ERR_USER_NOT_FOUND=%d\n", PASSWD_ERR_USER_NOT_FOUND);
    fprintf(fp, "PASSWD_ERR_PASSWORD_NOT_MATCH=%d\n", PASSWD_ERR_PASSWORD_NOT_MATCH);
    fprintf(fp, "PASSWD_ERR_SHADOW_FILE=%d\n", PASSWD_ERR_SHADOW_FILE);
    fprintf(fp, "PASSWD_ERR_INVALID_MSG=%d\n", PASSWD_ERR_INVALID_MSG);
    fprintf(fp, "PASSWD_ERR_INSUFFICIENT_MEM=%d\n", PASSWD_ERR_INSUFFICIENT_MEM);
    fprintf(fp, "PASSWD_ERR_INVALID_OPCODE=%d\n", PASSWD_ERR_INVALID_OPCODE);
    fprintf(fp, "PASSWD_ERR_INVALID_USER=%d\n", PASSWD_ERR_INVALID_USER);
    fprintf(fp, "PASSWD_ERR_INVALID_PARAM=%d\n", PASSWD_ERR_INVALID_PARAM);
    fprintf(fp, "PASSWD_ERR_PASSWD_UPD_FAIL=%d\n", PASSWD_ERR_PASSWD_UPD_FAIL);
    fprintf(fp, "PASSWD_ERR_SEND_FAILED=%d\n", PASSWD_ERR_SEND_FAILED);
    fputs("\n", fp);

    /* write message structure information */
    fputs("# message structure information\n", fp);
    fputs("\n", fp);

    /* write opcode info */
    fputs("# opcode\n", fp);
    fputs("[op_code_msg]\n", fp);
    fputs("PASSWD_SOCK_MSG_OPCODE_TYPE=integer\n", fp);
    fprintf(fp, "PASSWD_SOCK_MSG_OPCODE_SIZE=%d\n", (int)sizeof(int));
    fputs("\n", fp);

    /* write username info */
    fputs("# username info\n", fp);
    fputs("[msg_username]\n", fp);
    fputs("PASSWD_SOCK_MSG_UNAME_TYPE=string\n", fp);
    fprintf(fp, "PASSWD_SOCK_MSG_UNAME_SIZE=%d\n", PASSWD_USERNAME_SIZE);
    fputs("\n", fp);

    /* write old password info */
    fputs("# password info\n", fp);
    fputs("[msg_old_password]\n", fp);
    fputs("PASSWD_SOCK_MSG_OLDPASS_TYPE=string\n", fp);
    fprintf(fp, "PASSWD_SOCK_MSG_OLDPASS_SIZE=%d\n", PASSWD_PASSWORD_SIZE);
    fputs("\n", fp);

    /* write new password info */
    fputs("# password info\n", fp);
    fputs("[msg_new_password]\n", fp);
    fputs("PASSWD_SOCK_MSG_NEWPASS_TYPE=string\n", fp);
    fprintf(fp, "PASSWD_SOCK_MSG_NEWPASS_SIZE=%d\n", PASSWD_PASSWORD_SIZE);
    fputs("\n", fp);

    fclose(fp);
    return 0;
}
