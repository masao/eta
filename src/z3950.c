/*
 *
 */

#include "z3950.h"

#define C_PROMPT "Z> "

static ODR out, in, print;              /* encoding and decoding streams */
static FILE *apdu_file = 0;
static COMSTACK conn = 0;               /* our z-association */
static Z_IdAuthentication *auth = 0;    /* our current auth definition */
char *databaseNames[128];
int num_databaseNames = 0;
static Z_External *record_last = 0;
static int setnumber = 0;               /* current result set number */
static int smallSetUpperBound = 0;
static int largeSetLowerBound = 1;
static int mediumSetPresentNumber = 0;
static Z_ElementSetNames *elementSetNames = 0; 
static int setno = 1;                   /* current set offset */
static enum oid_proto protocol = PROTO_Z3950;      /* current app protocol */
static enum oid_value recordsyntax = VAL_USMARC;
static enum oid_value schema = VAL_NONE;
static int sent_close = 0;
static NMEM session_mem = NULL;      /* memory handle for init-response */
static Z_InitResponse *session = 0;     /* session parameters */
static char last_scan_line[512] = "0";
static char last_scan_query[512] = "0";
static char ccl_fields[512] = "default.bib";
static char* esPackageName = 0;

static char last_cmd[100] = "?";
static FILE *marcdump = 0;
static char *refid = NULL;

typedef enum {
    QueryType_Prefix,
    QueryType_CCL,
    QueryType_CCL2RPN
} QueryType;

static QueryType queryType = QueryType_Prefix;

#if YAZ_MODULE_ccl
static CCL_bibset bibset;               /* CCL bibset handle */
#endif

ODR getODROutputStream()
{
    return out;
}

void send_apdu(Z_APDU *a)
{
    char *buf;
    int len;

    if (apdu_file)
    {
        z_APDU(print, &a, 0, 0);
        odr_reset(print);
    }
    if (!z_APDU(out, &a, 0, 0))
    {
        odr_perror(out, "Encoding APDU");
        exit(1);
    }
    buf = odr_getbuf(out, &len, 0);
    /* printf ("sending APDU of size %d\n", len); */
    if (cs_put(conn, buf, len) < 0)
    {
        fprintf(stderr, "cs_put: %s", cs_errmsg(cs_errno(conn)));
        exit(1);
    }
    odr_reset(out); /* release the APDU structure  */
}

static void print_refid (Z_ReferenceId *id)
{
    if (id)
    {
        printf ("ReferenceId: '%.*s'\n", id->len, id->buf);
    }
}

static Z_ReferenceId *set_refid (ODR out)
{
    Z_ReferenceId *id;
    if (!refid)
        return 0;
    id = (Z_ReferenceId *) odr_malloc (out, sizeof(*id));
    id->size = id->len = strlen(refid);
    id->buf = (unsigned char *) odr_malloc (out, id->len);
    memcpy (id->buf, refid, id->len);
    return id;
}   

/* INIT SERVICE ------------------------------- */

static void send_initRequest()
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_initRequest);
    Z_InitRequest *req = apdu->u.initRequest;

    ODR_MASK_SET(req->options, Z_Options_search);
    ODR_MASK_SET(req->options, Z_Options_present);
    ODR_MASK_SET(req->options, Z_Options_namedResultSets);
    ODR_MASK_SET(req->options, Z_Options_triggerResourceCtrl);
    ODR_MASK_SET(req->options, Z_Options_scan);
    ODR_MASK_SET(req->options, Z_Options_sort);
    ODR_MASK_SET(req->options, Z_Options_extendedServices);
    ODR_MASK_SET(req->options, Z_Options_delSet);

    ODR_MASK_SET(req->protocolVersion, Z_ProtocolVersion_1);
    ODR_MASK_SET(req->protocolVersion, Z_ProtocolVersion_2);
    ODR_MASK_SET(req->protocolVersion, Z_ProtocolVersion_3);

    *req->maximumRecordSize = 1024*1024;
    *req->preferredMessageSize = 1024*1024;

    req->idAuthentication = auth;

    req->referenceId = set_refid (out);

    send_apdu(apdu);
    printf("Sent initrequest.\n");
}

static int process_initResponse(Z_InitResponse *res)
{
    /* save session parameters for later use */
    session_mem = odr_extract_mem(in);
    session = res;

    if (!*res->result)
        printf("Connection rejected by target.\n");
    else
        printf("Connection accepted by target.\n");
    if (res->implementationId)
        printf("ID     : %s\n", res->implementationId);
    if (res->implementationName)
        printf("Name   : %s\n", res->implementationName);
    if (res->implementationVersion)
        printf("Version: %s\n", res->implementationVersion);
    if (res->userInformationField)
    {
        printf("UserInformationfield:\n");
        if (!z_External(print, (Z_External**)&res-> userInformationField,
            0, 0))
        {
            odr_perror(print, "Printing userinfo\n");
            odr_reset(print);
        }
        if (res->userInformationField->which == Z_External_octet)
        {
            printf("Guessing visiblestring:\n");
            printf("'%s'\n", res->userInformationField->u. octet_aligned->buf);
        }
        odr_reset (print);
    }
    printf ("Options:");
    if (ODR_MASK_GET(res->options, Z_Options_search))
        printf (" search");
    if (ODR_MASK_GET(res->options, Z_Options_present))
        printf (" present");
    if (ODR_MASK_GET(res->options, Z_Options_delSet))
        printf (" delSet");
    if (ODR_MASK_GET(res->options, Z_Options_resourceReport))
        printf (" resourceReport");
    if (ODR_MASK_GET(res->options, Z_Options_resourceCtrl))
        printf (" resourceCtrl");
    if (ODR_MASK_GET(res->options, Z_Options_accessCtrl))
        printf (" accessCtrl");
    if (ODR_MASK_GET(res->options, Z_Options_scan))
        printf (" scan");
    if (ODR_MASK_GET(res->options, Z_Options_sort))
        printf (" sort");
    if (ODR_MASK_GET(res->options, Z_Options_extendedServices))
        printf (" extendedServices");
    if (ODR_MASK_GET(res->options, Z_Options_level_1Segmentation))
        printf (" level1Segmentation");
    if (ODR_MASK_GET(res->options, Z_Options_level_2Segmentation))
        printf (" level2Segmentation");
    if (ODR_MASK_GET(res->options, Z_Options_concurrentOperations))
        printf (" concurrentOperations");
    if (ODR_MASK_GET(res->options, Z_Options_namedResultSets))
        printf (" namedResultSets");
    printf ("\n");
    fflush (stdout);
    return 0;
}

int cmd_base(char *arg)
{
    int i;
    char *cp;

    if (!*arg)
    {
        printf("Usage: base <database> <database> ...\n");
        return 0;
    }
    for (i = 0; i<num_databaseNames; i++)
        xfree (databaseNames[i]);
    num_databaseNames = 0;
    while (1)
    {
        if (!(cp = strchr(arg, ' ')))
            cp = arg + strlen(arg);
        if (cp - arg < 1)
            break;
        databaseNames[num_databaseNames] = (char *)xmalloc (1 + cp - arg);
        memcpy (databaseNames[num_databaseNames], arg, cp - arg);
        databaseNames[num_databaseNames++][cp - arg] = '\0';
        if (!*cp)
            break;
        arg = cp+1;
    }
    return 1;
}


int cmd_open(char *arg)
{
    void *add;
    char type_and_host[101], base[101];
    CS_TYPE t;

    if (conn)
    {
        printf("Already connected.\n");

        cs_close (conn);
        conn = NULL;
        if (session_mem)
        {
            nmem_destroy (session_mem);
            session_mem = NULL;
        }
    }
    t = tcpip_type;
    base[0] = '\0';
    if (sscanf (arg, "%100[^/]/%100s", type_and_host, base) < 1)
        return 0;

    conn = cs_create_host(type_and_host, 1, &add);
    if (!conn)
    {
	printf ("Couldn't create comstack\n");
	return 0;
    }
    printf("Connecting...");
    fflush(stdout);
    if (cs_connect(conn, add) < 0)
    {
	printf ("error = %s\n", cs_strerror(conn));
	if (conn->cerrno == CSYSERR)
	    perror("system");
        cs_close(conn);
        conn = 0;
        return 0;
    }
    printf("Ok.\n");
    send_initRequest();
    if (*base)
        cmd_base (base);
    return 2;
}

int cmd_authentication(char *arg)
{
    static Z_IdAuthentication au;
    static char open[256];

    if (!*arg)
    {
        printf("Auth field set to null\n");
        auth = 0;
        return 1;
    }
    auth = &au;
    au.which = Z_IdAuthentication_open;
    au.u.open = open;
    strcpy(open, arg);
    return 1;
}

/* SEARCH SERVICE ------------------------------ */

static void display_variant(Z_Variant *v, int level)
{
    int i;

    for (i = 0; i < v->num_triples; i++)
    {
        printf("%*sclass=%d,type=%d", level * 4, "", *v->triples[i]->zclass,
            *v->triples[i]->type);
        if (v->triples[i]->which == Z_Triple_internationalString)
            printf(",value=%s\n", v->triples[i]->value.internationalString);
        else
            printf("\n");
    }
}

static void display_grs1(Z_GenericRecord *r, int level)
{
    int i;

    if (!r)
        return;
    for (i = 0; i < r->num_elements; i++)
    {
        Z_TaggedElement *t;

        printf("%*s", level * 4, "");
        t = r->elements[i];
        printf("(");
        if (t->tagType)
            printf("%d,", *t->tagType);
        else
            printf("?,");
        if (t->tagValue->which == Z_StringOrNumeric_numeric)
            printf("%d) ", *t->tagValue->u.numeric);
        else
            printf("%s) ", t->tagValue->u.string);
        if (t->content->which == Z_ElementData_subtree)
        {
            printf("\n");
            display_grs1(t->content->u.subtree, level+1);
        }
        else if (t->content->which == Z_ElementData_string)
            printf("%s\n", t->content->u.string);
        else if (t->content->which == Z_ElementData_numeric)
            printf("%d\n", *t->content->u.numeric);
        else if (t->content->which == Z_ElementData_oid)
        {
            int *ip = t->content->u.oid;
            oident *oent;

            if ((oent = oid_getentbyoid(t->content->u.oid)))
                printf("OID: %s\n", oent->desc);
            else
            {
                printf("{");
                while (ip && *ip >= 0)
                    printf(" %d", *(ip++));
                printf(" }\n");
            }
        }
        else if (t->content->which == Z_ElementData_noDataRequested)
            printf("[No data requested]\n");
        else if (t->content->which == Z_ElementData_elementEmpty)
            printf("[Element empty]\n");
        else if (t->content->which == Z_ElementData_elementNotThere)
            printf("[Element not there]\n");
        else
            printf("??????\n");
        if (t->appliedVariant)
            display_variant(t->appliedVariant, level+1);
        if (t->metaData && t->metaData->supportedVariants)
        {
            int c;

            printf("%*s---- variant list\n", (level+1)*4, "");
            for (c = 0; c < t->metaData->num_supportedVariants; c++)
            {
                printf("%*svariant #%d\n", (level+1)*4, "", c);
                display_variant(t->metaData->supportedVariants[c], level + 2);
            }
        }
    }
}

static void print_record(const unsigned char *buf, size_t len)
{
   size_t i;
   for (i = 0; i<len; i++)
       if ((buf[i] <= 126 && buf[i] >= 32) || strchr ("\n\r\t\f", buf[i]))
           fputc (buf[i], stdout);
       else
           printf ("\\X%02X", buf[i]);
   /* add newline if not already added ... */
   if (i <= 0 || buf[i-1] != '\n')
       fputc ('\n', stdout);
}

static void display_record(Z_DatabaseRecord *p)
{
    Z_External *r = (Z_External*) p;
    oident *ent = oid_getentbyoid(r->direct_reference);

    record_last = r;
    /*
     * Tell the user what we got.
     */
    if (r->direct_reference)
    {
        printf("Record type: ");
        if (ent)
            printf("%s\n", ent->desc);
        else if (!odr_oid(print, &r->direct_reference, 0, 0))
        {
            odr_perror(print, "print oid");
            odr_reset(print);
        }
    }
    /* Check if this is a known, ASN.1 type tucked away in an octet string */
    if (ent && r->which == Z_External_octet)
    {
        Z_ext_typeent *type = z_ext_getentbyref(ent->value);
        void *rr;

        if (type)
        {
            /*
             * Call the given decoder to process the record.
             */
            odr_setbuf(in, (char*)p->u.octet_aligned->buf,
                p->u.octet_aligned->len, 0);
            if (!(*type->fun)(in, (char **)&rr, 0, 0))
            {
                odr_perror(in, "Decoding constructed record.");
                fprintf(stderr, "[Near %d]\n", odr_offset(in));
                fprintf(stderr, "Packet dump:\n---------\n");
                odr_dumpBER(stderr, (char*)p->u.octet_aligned->buf,
                    p->u.octet_aligned->len);
                fprintf(stderr, "---------\n");
                exit(1);
            }
            /*
             * Note: we throw away the original, BER-encoded record here.
             * Do something else with it if you want to keep it.
             */
            r->u.sutrs = (Z_SUTRS *) rr; /* we don't actually check the type here. */
            r->which = type->what;
        }
    }
    if (ent && ent->value == VAL_SOIF)
        print_record((const unsigned char *) r->u.octet_aligned->buf, r->u.octet_aligned->len);
    else if (r->which == Z_External_octet && p->u.octet_aligned->len)
    {
        const char *octet_buf = (char*)p->u.octet_aligned->buf;
        if (ent->value == VAL_TEXT_XML || ent->value == VAL_APPLICATION_XML ||
            ent->value == VAL_HTML)
            print_record((const unsigned char *) octet_buf,
                         p->u.octet_aligned->len);
        else
        {
            if (marc_display (octet_buf, NULL) <= 0)
            {
                printf ("ISO2709 decoding failed, dumping record as is:\n");
                print_record((const unsigned char*) octet_buf,
                              p->u.octet_aligned->len);
            }
        }
        if (marcdump)
            fwrite (octet_buf, 1, p->u.octet_aligned->len, marcdump);
    }
    else if (ent && ent->value == VAL_SUTRS)
    {
        if (r->which != Z_External_sutrs)
        {
            printf("Expecting single SUTRS type for SUTRS.\n");
            return;
        }
        print_record(r->u.sutrs->buf, r->u.sutrs->len);
    }
    else if (ent && ent->value == VAL_GRS1)
    {
        if (r->which != Z_External_grs1)
        {
            printf("Expecting single GRS type for GRS.\n");
            return;
        }
        display_grs1(r->u.grs1, 0);
    }
    else 
    {
        printf("Unknown record representation.\n");
        if (!z_External(print, &r, 0, 0))
        {
            odr_perror(print, "Printing external");
            odr_reset(print);
        }
    }
}


static void display_diagrecs(Z_DiagRec **pp, int num)
{
    int i;
    oident *ent;
    Z_DefaultDiagFormat *r;

    printf("Diagnostic message(s) from database:\n");
    for (i = 0; i<num; i++)
    {
        Z_DiagRec *p = pp[i];
        if (p->which != Z_DiagRec_defaultFormat)
        {
            printf("Diagnostic record not in default format.\n");
            return;
        }
        else
            r = p->u.defaultFormat;
        if (!(ent = oid_getentbyoid(r->diagnosticSetId)) ||
            ent->oclass != CLASS_DIAGSET || ent->value != VAL_BIB1)
            printf("Missing or unknown diagset\n");
        printf("    [%d] %s", *r->condition, diagbib1_str(*r->condition));
#ifdef ASN_COMPILED
        switch (r->which)
        {
        case Z_DefaultDiagFormat_v2Addinfo:
            printf (" -- v2 addinfo '%s'\n", r->u.v2Addinfo);
            break;
        case Z_DefaultDiagFormat_v3Addinfo:
            printf (" -- v3 addinfo '%s'\n", r->u.v3Addinfo);
            break;
        }
#else
        if (r->addinfo && *r->addinfo)
            printf(" -- '%s'\n", r->addinfo);
        else
            printf("\n");
#endif
    }
}


static void display_nameplusrecord(Z_NamePlusRecord *p)
{
    if (p->databaseName)
        printf("[%s]", p->databaseName);
    if (p->which == Z_NamePlusRecord_surrogateDiagnostic)
        display_diagrecs(&p->u.surrogateDiagnostic, 1);
    else if (p->which == Z_NamePlusRecord_databaseRecord)
        display_record(p->u.databaseRecord);
}

static void display_records(Z_Records *p)
{
    int i;

    if (p->which == Z_Records_NSD)
    {
#ifdef ASN_COMPILED
        Z_DiagRec dr, *dr_p = &dr;
        dr.which = Z_DiagRec_defaultFormat;
        dr.u.defaultFormat = p->u.nonSurrogateDiagnostic;
        display_diagrecs (&dr_p, 1);
#else
        display_diagrecs (&p->u.nonSurrogateDiagnostic, 1);
#endif
    }
    else if (p->which == Z_Records_multipleNSD)
        display_diagrecs (p->u.multipleNonSurDiagnostics->diagRecs,
                          p->u.multipleNonSurDiagnostics->num_diagRecs);
    else 
    {
        printf("Records: %d\n", p->u.databaseOrSurDiagnostics->num_records);
        for (i = 0; i < p->u.databaseOrSurDiagnostics->num_records; i++)
            display_nameplusrecord(p->u.databaseOrSurDiagnostics->records[i]);
    }
}

static int send_deleteResultSetRequest(char *arg)
{
    char names[8][32];
    int i;

    Z_APDU *apdu = zget_APDU(out, Z_APDU_deleteResultSetRequest);
    Z_DeleteResultSetRequest *req = apdu->u.deleteResultSetRequest;

    req->referenceId = set_refid (out);

    req->num_resultSetList =
        sscanf (arg, "%30s %30s %30s %30s %30s %30s %30s %30s",
                names[0], names[1], names[2], names[3],
                names[4], names[5], names[6], names[7]);

    req->deleteFunction = (int *)
        odr_malloc (out, sizeof(*req->deleteFunction));
    if (req->num_resultSetList > 0)
    {
        *req->deleteFunction = Z_DeleteRequest_list;
        req->resultSetList = (char **)
            odr_malloc (out, sizeof(*req->resultSetList)*
                        req->num_resultSetList);
        for (i = 0; i<req->num_resultSetList; i++)
            req->resultSetList[i] = names[i];
    }
    else
    {
        *req->deleteFunction = Z_DeleteRequest_all;
        req->resultSetList = 0;
    }
    
    send_apdu(apdu);
    printf("Sent deleteResultSetRequest.\n");
    return 2;
}

static int send_searchRequest(char *arg)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_searchRequest);
    Z_SearchRequest *req = apdu->u.searchRequest;
    Z_Query query;
    int oid[OID_SIZE];
#if YAZ_MODULE_ccl
    struct ccl_rpn_node *rpn = NULL;
    int error, pos;
#endif
    char setstring[100];
    Z_RPNQuery *RPNquery;
    Odr_oct ccl_query;

#if YAZ_MODULE_ccl
    if (queryType == QueryType_CCL2RPN)
    {
        rpn = ccl_find_str(bibset, arg, &error, &pos);
        if (error)
        {
            printf("CCL ERROR: %s\n", ccl_err_msg(error));
            return 0;
        }
    }
#endif
    req->referenceId = set_refid (out);
    if (!strcmp(arg, "@big")) /* strictly for troublemaking */
    {
        static unsigned char big[2100];
        static Odr_oct bigo;

        /* send a very big referenceid to test transport stack etc. */
        memset(big, 'A', 2100);
        bigo.len = bigo.size = 2100;
        bigo.buf = big;
        req->referenceId = &bigo;
    }
    
    if (setnumber >= 0)
    {
        sprintf(setstring, "%d", ++setnumber);
        req->resultSetName = setstring;
    }
    *req->smallSetUpperBound = smallSetUpperBound;
    *req->largeSetLowerBound = largeSetLowerBound;
    *req->mediumSetPresentNumber = mediumSetPresentNumber;
    if (smallSetUpperBound > 0 || (largeSetLowerBound > 1 &&
        mediumSetPresentNumber > 0))
    {
        oident prefsyn;

        prefsyn.proto = protocol;
        prefsyn.oclass = CLASS_RECSYN;
        prefsyn.value = recordsyntax;
        req->preferredRecordSyntax =
            odr_oiddup(out, oid_ent_to_oid(&prefsyn, oid));
        req->smallSetElementSetNames =
            req->mediumSetElementSetNames = elementSetNames;
    }
    req->num_databaseNames = num_databaseNames;
    req->databaseNames = databaseNames;

    req->query = &query;

    switch (queryType)
    {
    case QueryType_Prefix:
        query.which = Z_Query_type_1;
        RPNquery = p_query_rpn (out, protocol, arg);
        if (!RPNquery)
        {
            printf("Prefix query error\n");
            return 0;
        }
        query.u.type_1 = RPNquery;
        break;
    case QueryType_CCL:
        query.which = Z_Query_type_2;
        query.u.type_2 = &ccl_query;
        ccl_query.buf = (unsigned char*) arg;
        ccl_query.len = strlen(arg);
        break;
#if YAZ_MODULE_ccl
    case QueryType_CCL2RPN:
        query.which = Z_Query_type_1;
        RPNquery = ccl_rpn_query(out, rpn);
        if (!RPNquery)
        {
            printf ("Couldn't convert from CCL to RPN\n");
            return 0;
        }
        query.u.type_1 = RPNquery;
        ccl_rpn_delete (rpn);
        break;
#endif
    default:
        printf ("Unsupported query type\n");
        return 0;
    }
    send_apdu(apdu);
    setno = 1;
    printf("Sent searchRequest.\n");
    return 2;
}

static int process_searchResponse(Z_SearchResponse *res)
{
    printf ("Received SearchResponse.\n");
    print_refid (res->referenceId);
    if (*res->searchStatus)
        printf("Search was a success.\n");
    else
        printf("Search was a bloomin' failure.\n");

    printf("Number of hits: %d, setno %d\n",
        *res->resultCount, setnumber);

    printf("records returned: %d\n",
        *res->numberOfRecordsReturned);
    setno += *res->numberOfRecordsReturned;
    if (res->records)
        display_records(res->records);

    return 0;
}

static void print_level(int iLevel)
{
    int i;
    for (i = 0; i < iLevel * 4; i++)
        printf(" ");
}

static void print_int(int iLevel, const char *pTag, int *pInt)
{
    if (pInt != NULL)
    {
        print_level(iLevel);
        printf("%s: %d\n", pTag, *pInt);
    }
}

static void print_string(int iLevel, const char *pTag, const char *pString)
{
    if (pString != NULL)
    {
        print_level(iLevel);
        printf("%s: %s\n", pTag, pString);
    }
}

static void print_oid(int iLevel, const char *pTag, Odr_oid *pOid)
{
    if (pOid != NULL)
    {
        int *pInt = pOid;

        print_level(iLevel);
        printf("%s:", pTag);
        for (; *pInt != -1; pInt++)
            printf(" %d", *pInt);
        printf("\n");
    }
}

static void print_referenceId(int iLevel, Z_ReferenceId *referenceId)
{
    if (referenceId != NULL)
    {
        int i;

        print_level(iLevel);
        printf("Ref Id (%d, %d): ", referenceId->len, referenceId->size);
        for (i = 0; i < referenceId->len; i++)
            printf("%c", referenceId->buf[i]);
        printf("\n");
    }
}

static void print_string_or_numeric(int iLevel, const char *pTag, Z_StringOrNumeric *pStringNumeric)
{
    if (pStringNumeric != NULL)
    {
        switch (pStringNumeric->which)
        {
            case Z_StringOrNumeric_string:
                print_string(iLevel, pTag, pStringNumeric->u.string);
                break;

            case Z_StringOrNumeric_numeric:
                print_int(iLevel, pTag, pStringNumeric->u.numeric);
                break;

            default:
                print_level(iLevel);
                printf("%s: valid type for Z_StringOrNumeric\n", pTag);
                break;
        }
    }
}

static void print_universe_report_duplicate(int iLevel, Z_UniverseReportDuplicate *pUniverseReportDuplicate)
{
    if (pUniverseReportDuplicate != NULL)
    {
        print_level(iLevel);
        printf("Universe Report Duplicate: \n");
        iLevel++;
        print_string_or_numeric(iLevel, "Hit No", pUniverseReportDuplicate->hitno);
    }
}

static void print_universe_report_hits(int iLevel, Z_UniverseReportHits *pUniverseReportHits)
{
    if (pUniverseReportHits != NULL)
    {
        print_level(iLevel);
        printf("Universe Report Hits: \n");
        iLevel++;
        print_string_or_numeric(iLevel, "Database", pUniverseReportHits->database);
        print_string_or_numeric(iLevel, "Hits", pUniverseReportHits->hits);
    }
}

static void print_universe_report(int iLevel, Z_UniverseReport *pUniverseReport)
{
    if (pUniverseReport != NULL)
    {
        print_level(iLevel);
        printf("Universe Report: \n");
        iLevel++;
        print_int(iLevel, "Total Hits", pUniverseReport->totalHits);
        switch (pUniverseReport->which)
        {
            case Z_UniverseReport_databaseHits:
                print_universe_report_hits(iLevel, pUniverseReport->u.databaseHits);
                break;

            case Z_UniverseReport_duplicate:
                print_universe_report_duplicate(iLevel, pUniverseReport->u.duplicate);
                break;

            default:
                print_level(iLevel);
                printf("Type: %d\n", pUniverseReport->which);
                break;
        }
    }
}

static void print_external(int iLevel, Z_External *pExternal)
{
    if (pExternal != NULL)
    {
        print_level(iLevel);
        printf("External: \n");
        iLevel++;
        print_oid(iLevel, "Direct Reference", pExternal->direct_reference);
        print_int(iLevel, "InDirect Reference", pExternal->indirect_reference);
        print_string(iLevel, "Descriptor", pExternal->descriptor);
        switch (pExternal->which)
        {
            case Z_External_universeReport:
                print_universe_report(iLevel, pExternal->u.universeReport);
                break;

            default:
                print_level(iLevel);
                printf("Type: %d\n", pExternal->which);
                break;
        }
    }
}

static int process_resourceControlRequest (Z_ResourceControlRequest *req)
{
    printf ("Received ResourceControlRequest.\n");
    print_referenceId(1, req->referenceId);
    print_int(1, "Suspended Flag", req->suspendedFlag);
    print_int(1, "Partial Results Available", req->partialResultsAvailable);
    print_int(1, "Response Required", req->responseRequired);
    print_int(1, "Triggered Request Flag", req->triggeredRequestFlag);
    print_external(1, req->resourceReport);
    return 0;
}

void process_ESResponse(Z_ExtendedServicesResponse *res)
{
    printf("process_ESResponse status=");
    switch (*res->operationStatus)
    {
    case Z_ExtendedServicesResponse_done:
        printf ("done\n");
        break;
    case Z_ExtendedServicesResponse_accepted:
        printf ("accepted\n");
        break;
    case Z_ExtendedServicesResponse_failure:
        printf ("failure\n");
        display_diagrecs(res->diagnostics, res->num_diagnostics);
        break;
    }
    if ( (*res->operationStatus != Z_ExtendedServicesResponse_failure) &&
        (res->num_diagnostics != 0) ) {
        display_diagrecs(res->diagnostics, res->num_diagnostics);
    }

}

#if YAZ_MODULE_ill

const char *get_ill_element (void *clientData, const char *element)
{
    printf ("%s\n", element);
    return 0;
}

static Z_External *create_external_itemRequest()
{
    struct ill_get_ctl ctl;
    ILL_ItemRequest *req;
    Z_External *r = 0;
    int item_request_size = 0;
    char *item_request_buf = 0;

    ctl.odr = out;
    ctl.clientData = 0;
    ctl.f = get_ill_element;

    req = ill_get_ItemRequest(&ctl, "ill", 0);
    if (!req)
        printf ("ill_get_ItemRequest failed\n");
        
    if (!ill_ItemRequest (out, &req, 0, 0))
    {
        if (apdu_file)
        {
            ill_ItemRequest(print, &req, 0, 0);
            odr_reset(print);
        }
        item_request_buf = odr_getbuf (out, &item_request_size, 0);
        if (item_request_buf)
            odr_setbuf (out, item_request_buf, item_request_size, 1);
        printf ("Couldn't encode ItemRequest, size %d\n", item_request_size);
        return 0;
    }
    else
    {
        oident oid;
        
        item_request_buf = odr_getbuf (out, &item_request_size, 0);
        oid.proto = PROTO_GENERAL;
        oid.oclass = CLASS_GENERAL;
        oid.value = VAL_ISO_ILL_1;
        
        r = (Z_External *) odr_malloc (out, sizeof(*r));
        r->direct_reference = odr_oiddup(out,oid_getoidbyent(&oid)); 
        r->indirect_reference = 0;
        r->descriptor = 0;
        r->which = Z_External_single;
        
        r->u.single_ASN1_type = (Odr_oct *)
            odr_malloc (out, sizeof(*r->u.single_ASN1_type));
        r->u.single_ASN1_type->buf = (unsigned char *)
	    odr_malloc (out, item_request_size);
        r->u.single_ASN1_type->len = item_request_size;
        r->u.single_ASN1_type->size = item_request_size;
        memcpy (r->u.single_ASN1_type->buf, item_request_buf,
                item_request_size);
        printf ("len = %d\n", item_request_size);
    }
    return r;
}
#endif

#ifdef YAZ_MODULE_ill
static Z_External *create_external_ILL_APDU(int which)
{
    struct ill_get_ctl ctl;
    ILL_APDU *ill_apdu;
    Z_External *r = 0;
    int ill_request_size = 0;
    char *ill_request_buf = 0;
        
    ctl.odr = out;
    ctl.clientData = 0;
    ctl.f = get_ill_element;

    ill_apdu = ill_get_APDU(&ctl, "ill", 0);

    if (!ill_APDU (out, &ill_apdu, 0, 0))
    {
        if (apdu_file)
        {
            printf ("-------------------\n");
            ill_APDU(print, &ill_apdu, 0, 0);
            odr_reset(print);
            printf ("-------------------\n");
        }
        ill_request_buf = odr_getbuf (out, &ill_request_size, 0);
        if (ill_request_buf)
            odr_setbuf (out, ill_request_buf, ill_request_size, 1);
        printf ("Couldn't encode ILL-Request, size %d\n", ill_request_size);
        return 0;
    }
    else
    {
        oident oid;
        ill_request_buf = odr_getbuf (out, &ill_request_size, 0);
        
        oid.proto = PROTO_GENERAL;
        oid.oclass = CLASS_GENERAL;
        oid.value = VAL_ISO_ILL_1;
        
        r = (Z_External *) odr_malloc (out, sizeof(*r));
        r->direct_reference = odr_oiddup(out,oid_getoidbyent(&oid)); 
        r->indirect_reference = 0;
        r->descriptor = 0;
        r->which = Z_External_single;
        
        r->u.single_ASN1_type = (Odr_oct *)
            odr_malloc (out, sizeof(*r->u.single_ASN1_type));
        r->u.single_ASN1_type->buf = (unsigned char *)
	    odr_malloc (out, ill_request_size);
        r->u.single_ASN1_type->len = ill_request_size;
        r->u.single_ASN1_type->size = ill_request_size;
        memcpy (r->u.single_ASN1_type->buf, ill_request_buf, ill_request_size);
        printf ("len = %d\n", ill_request_size);
    }
    return r;
}
#endif


static Z_External *create_ItemOrderExternal(const char *type, int itemno)
{
    Z_External *r = (Z_External *) odr_malloc(out, sizeof(Z_External));
    oident ItemOrderRequest;
  
    ItemOrderRequest.proto = PROTO_Z3950;
    ItemOrderRequest.oclass = CLASS_EXTSERV;
    ItemOrderRequest.value = VAL_ITEMORDER;
 
    r->direct_reference = odr_oiddup(out,oid_getoidbyent(&ItemOrderRequest)); 
    r->indirect_reference = 0;
    r->descriptor = 0;

    r->which = Z_External_itemOrder;

    r->u.itemOrder = (Z_ItemOrder *) odr_malloc(out,sizeof(Z_ItemOrder));
    memset(r->u.itemOrder, 0, sizeof(Z_ItemOrder));
#ifdef ASN_COMPILED
    r->u.itemOrder->which=Z_IOItemOrder_esRequest;
#else
    r->u.itemOrder->which=Z_ItemOrder_esRequest;
#endif

    r->u.itemOrder->u.esRequest = (Z_IORequest *) 
        odr_malloc(out,sizeof(Z_IORequest));
    memset(r->u.itemOrder->u.esRequest, 0, sizeof(Z_IORequest));

    r->u.itemOrder->u.esRequest->toKeep = (Z_IOOriginPartToKeep *)
        odr_malloc(out,sizeof(Z_IOOriginPartToKeep));
    memset(r->u.itemOrder->u.esRequest->toKeep, 0, sizeof(Z_IOOriginPartToKeep));
    r->u.itemOrder->u.esRequest->notToKeep = (Z_IOOriginPartNotToKeep *)
        odr_malloc(out,sizeof(Z_IOOriginPartNotToKeep));
    memset(r->u.itemOrder->u.esRequest->notToKeep, 0, sizeof(Z_IOOriginPartNotToKeep));

    r->u.itemOrder->u.esRequest->toKeep->supplDescription = NULL;
    r->u.itemOrder->u.esRequest->toKeep->contact = NULL;
    r->u.itemOrder->u.esRequest->toKeep->addlBilling = NULL;

    r->u.itemOrder->u.esRequest->notToKeep->resultSetItem =
        (Z_IOResultSetItem *) odr_malloc(out, sizeof(Z_IOResultSetItem));
    memset(r->u.itemOrder->u.esRequest->notToKeep->resultSetItem, 0, sizeof(Z_IOResultSetItem));
    r->u.itemOrder->u.esRequest->notToKeep->resultSetItem->resultSetId = "1";

    r->u.itemOrder->u.esRequest->notToKeep->resultSetItem->item =
        (int *) odr_malloc(out, sizeof(int));
    *r->u.itemOrder->u.esRequest->notToKeep->resultSetItem->item = itemno;

#if YAZ_MODULE_ill
    if (!strcmp (type, "item") || !strcmp(type, "2"))
    {
        printf ("using item-request\n");
        r->u.itemOrder->u.esRequest->notToKeep->itemRequest = 
            create_external_itemRequest();
    }
    else if (!strcmp(type, "ill") || !strcmp(type, "1"))
    {
        printf ("using ILL-request\n");
        r->u.itemOrder->u.esRequest->notToKeep->itemRequest = 
            create_external_ILL_APDU(ILL_APDU_ILL_Request);
    }
    else if (!strcmp(type, "xml") || !strcmp(type, "3"))
    {
	const char *xml_buf =
		"<itemorder>\n"
		"  <type>request</type>\n"
		"  <libraryNo>000200</libraryNo>\n"
		"  <borrowerTicketNo> 1212 </borrowerTicketNo>\n"
		"</itemorder>";
        r->u.itemOrder->u.esRequest->notToKeep->itemRequest =
            z_ext_record (out, VAL_TEXT_XML, xml_buf, strlen(xml_buf));
    }
    else
        r->u.itemOrder->u.esRequest->notToKeep->itemRequest = 0;

#else
    r->u.itemOrder->u.esRequest->notToKeep->itemRequest = 0;
#endif
    return r;
}

static int send_itemorder(const char *type, int itemno)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_extendedServicesRequest);
    Z_ExtendedServicesRequest *req = apdu->u.extendedServicesRequest;
    oident ItemOrderRequest;

    ItemOrderRequest.proto = PROTO_Z3950;
    ItemOrderRequest.oclass = CLASS_EXTSERV;
    ItemOrderRequest.value = VAL_ITEMORDER;
    req->packageType = odr_oiddup(out,oid_getoidbyent(&ItemOrderRequest));
    req->packageName = esPackageName;

    req->taskSpecificParameters = create_ItemOrderExternal(type, itemno);

    send_apdu(apdu);
    return 0;
}

int cmd_update(char *arg)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_extendedServicesRequest );
    Z_ExtendedServicesRequest *req = apdu->u.extendedServicesRequest;
    Z_External *r;
    int oid[OID_SIZE];
    Z_IUOriginPartToKeep *toKeep;
    Z_IUSuppliedRecords *notToKeep;
    oident update_oid;
    printf ("Update request\n");
    fflush(stdout);

    if (!record_last)
        return 0;
    update_oid.proto = PROTO_Z3950;
    update_oid.oclass = CLASS_EXTSERV;
    update_oid.value = VAL_DBUPDATE;
    oid_ent_to_oid (&update_oid, oid);
    req->packageType = odr_oiddup(out,oid);
    req->packageName = esPackageName;
    
    req->referenceId = set_refid (out);

    r = req->taskSpecificParameters = (Z_External *)
        odr_malloc (out, sizeof(*r));
    r->direct_reference = odr_oiddup(out,oid);
    r->indirect_reference = 0;
    r->descriptor = 0;
    r->which = Z_External_update;
    r->u.update = (Z_IUUpdate *) odr_malloc(out, sizeof(*r->u.update));
    r->u.update->which = Z_IUUpdate_esRequest;
    r->u.update->u.esRequest = (Z_IUUpdateEsRequest *)
        odr_malloc(out, sizeof(*r->u.update->u.esRequest));
    toKeep = r->u.update->u.esRequest->toKeep = (Z_IUOriginPartToKeep *)
        odr_malloc(out, sizeof(*r->u.update->u.esRequest->toKeep));
    toKeep->databaseName = databaseNames[0];
    toKeep->schema = 0;
    toKeep->elementSetName = 0;
    toKeep->actionQualifier = 0;
    toKeep->action = (int *) odr_malloc(out, sizeof(*toKeep->action));
    *toKeep->action = Z_IUOriginPartToKeep_recordInsert;

    notToKeep = r->u.update->u.esRequest->notToKeep = (Z_IUSuppliedRecords *)
        odr_malloc(out, sizeof(*r->u.update->u.esRequest->notToKeep));
    notToKeep->num = 1;
    notToKeep->elements = (Z_IUSuppliedRecords_elem **)
        odr_malloc(out, sizeof(*notToKeep->elements));
    notToKeep->elements[0] = (Z_IUSuppliedRecords_elem *)
        odr_malloc(out, sizeof(**notToKeep->elements));
    notToKeep->elements[0]->u.number = 0;
    notToKeep->elements[0]->supplementalId = 0;
    notToKeep->elements[0]->correlationInfo = 0;
    notToKeep->elements[0]->record = record_last;
    
    send_apdu(apdu);

    return 2;
}

/* II : Added to do DALI Item Order Extended services request */
int cmd_itemorder(char *arg)
{
    char type[12];
    int itemno;

    if (sscanf (arg, "%10s %d", type, &itemno) != 2)
        return 0;

    printf("Item order request\n");
    fflush(stdout);
    send_itemorder(type, itemno);
    return(2);
}

int cmd_find(char *arg)
{
    if (!*arg)
    {
        printf("Find what?\n");
        return 0;
    }
    if (!conn)
    {
        printf("Not connected yet\n");
        return 0;
    }
    if (!send_searchRequest(arg))
        return 0;
    return 2;
}

int cmd_delete(char *arg)
{
    if (!conn)
    {
        printf("Not connected yet\n");
        return 0;
    }
    if (!send_deleteResultSetRequest(arg))
        return 0;
    return 2;
}

int cmd_ssub(char *arg)
{
    if (!(smallSetUpperBound = atoi(arg)))
        return 0;
    return 1;
}

int cmd_lslb(char *arg)
{
    if (!(largeSetLowerBound = atoi(arg)))
        return 0;
    return 1;
}

int cmd_mspn(char *arg)
{
    if (!(mediumSetPresentNumber = atoi(arg)))
        return 0;
    return 1;
}

int cmd_status(char *arg)
{
    printf("smallSetUpperBound: %d\n", smallSetUpperBound);
    printf("largeSetLowerBound: %d\n", largeSetLowerBound);
    printf("mediumSetPresentNumber: %d\n", mediumSetPresentNumber);
    return 1;
}

int cmd_setnames(char *arg)
{
    if (setnumber < 0)
    {
        printf("Set numbering enabled.\n");
        setnumber = 0;
    }
    else
    {
        printf("Set numbering disabled.\n");
        setnumber = -1;
    }
    return 1;
}

/* PRESENT SERVICE ----------------------------- */

static int send_presentRequest(char *arg)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_presentRequest);
    Z_PresentRequest *req = apdu->u.presentRequest;
    Z_RecordComposition compo;
    oident prefsyn;
    int nos = 1;
    int oid[OID_SIZE];
    char *p;
    char setstring[100];

    req->referenceId = set_refid (out);
    if ((p = strchr(arg, '+')))
    {
        nos = atoi(p + 1);
        *p = 0;
    }
    if (*arg)
        setno = atoi(arg);
    if (p && (p=strchr(p+1, '+')))
    {
        strcpy (setstring, p+1);
        req->resultSetId = setstring;
    }
    else if (setnumber >= 0)
    {
        sprintf(setstring, "%d", setnumber);
        req->resultSetId = setstring;
    }
#if 0
    if (1)
    {
        static Z_Range range;
        static Z_Range *rangep = &range;
    req->num_ranges = 1;
#endif
    req->resultSetStartPoint = &setno;
    req->numberOfRecordsRequested = &nos;
    prefsyn.proto = protocol;
    prefsyn.oclass = CLASS_RECSYN;
    prefsyn.value = recordsyntax;
    req->preferredRecordSyntax =
        odr_oiddup (out, oid_ent_to_oid(&prefsyn, oid));

    if (schema != VAL_NONE)
    {
        oident prefschema;

        prefschema.proto = protocol;
        prefschema.oclass = CLASS_SCHEMA;
        prefschema.value = schema;

        req->recordComposition = &compo;
        compo.which = Z_RecordComp_complex;
        compo.u.complex = (Z_CompSpec *)
            odr_malloc(out, sizeof(*compo.u.complex));
        compo.u.complex->selectAlternativeSyntax = (bool_t *) 
            odr_malloc(out, sizeof(bool_t));
        *compo.u.complex->selectAlternativeSyntax = 0;

        compo.u.complex->generic = (Z_Specification *)
            odr_malloc(out, sizeof(*compo.u.complex->generic));
        compo.u.complex->generic->schema = (Odr_oid *)
            odr_oiddup(out, oid_ent_to_oid(&prefschema, oid));
        if (!compo.u.complex->generic->schema)
        {
            /* OID wasn't a schema! Try record syntax instead. */
            prefschema.oclass = CLASS_RECSYN;
            compo.u.complex->generic->schema = (Odr_oid *)
                odr_oiddup(out, oid_ent_to_oid(&prefschema, oid));
        }
        if (!elementSetNames)
            compo.u.complex->generic->elementSpec = 0;
        else
        {
            compo.u.complex->generic->elementSpec = (Z_ElementSpec *)
                odr_malloc(out, sizeof(Z_ElementSpec));
            compo.u.complex->generic->elementSpec->which =
                Z_ElementSpec_elementSetName;
            compo.u.complex->generic->elementSpec->u.elementSetName =
                elementSetNames->u.generic;
        }
        compo.u.complex->num_dbSpecific = 0;
        compo.u.complex->dbSpecific = 0;
        compo.u.complex->num_recordSyntax = 0;
        compo.u.complex->recordSyntax = 0;
    }
    else if (elementSetNames)
    {
        req->recordComposition = &compo;
        compo.which = Z_RecordComp_simple;
        compo.u.simple = elementSetNames;
    }
    send_apdu(apdu);
    printf("Sent presentRequest (%d+%d).\n", setno, nos);
    return 2;
}

void process_close(Z_Close *req)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_close);
    Z_Close *res = apdu->u.close;

    static char *reasons[] =
    {
        "finished",
        "shutdown",
        "system problem",
        "cost limit reached",
        "resources",
        "security violation",
        "protocolError",
        "lack of activity",
        "peer abort",
        "unspecified"
    };

    printf("Reason: %s, message: %s\n", reasons[*req->closeReason],
        req->diagnosticInformation ? req->diagnosticInformation : "NULL");
    if (sent_close)
    {
        cs_close (conn);
        conn = NULL;
        if (session_mem)
        {
            nmem_destroy (session_mem);
            session_mem = NULL;
        }
        sent_close = 0;
    }
    else
    {
        *res->closeReason = Z_Close_finished;
        send_apdu(apdu);
        printf("Sent response.\n");
        sent_close = 1;
    }
}

int cmd_show(char *arg)
{
    if (!conn)
    {
        printf("Not connected yet\n");
        return 0;
    }
    if (!send_presentRequest(arg))
        return 0;
    return 2;
}

int cmd_quit(char *arg)
{
    printf("See you later, alligator.\n");
    exit(0);
    return 0;
}

int cmd_cancel(char *arg)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_triggerResourceControlRequest);
    Z_TriggerResourceControlRequest *req =
        apdu->u.triggerResourceControlRequest;
    bool_t rfalse = 0;
    
    if (!conn)
    {
        printf("Session not initialized yet\n");
        return 0;
    }
    if (!ODR_MASK_GET(session->options, Z_Options_triggerResourceCtrl))
    {
        printf("Target doesn't support cancel (trigger resource ctrl)\n");
        return 0;
    }
    *req->requestedAction = Z_TriggerResourceCtrl_cancel;
    req->resultSetWanted = &rfalse;

    send_apdu(apdu);
    printf("Sent cancel request\n");
    return 2;
}

int send_scanrequest(const char *query, int pp, int num, const char *term)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_scanRequest);
    Z_ScanRequest *req = apdu->u.scanRequest;
    int use_rpn = 1;
#if YAZ_MODULE_ccl
    int oid[OID_SIZE];
    
    if (queryType == QueryType_CCL2RPN)
    {
        oident bib1;
        int error, pos;
        struct ccl_rpn_node *rpn;

        rpn = ccl_find_str (bibset,  query, &error, &pos);
        if (error)
        {
            printf("CCL ERROR: %s\n", ccl_err_msg(error));
            return -1;
        }
        use_rpn = 0;
        bib1.proto = PROTO_Z3950;
        bib1.oclass = CLASS_ATTSET;
        bib1.value = VAL_BIB1;
        req->attributeSet = oid_ent_to_oid (&bib1, oid);
        if (!(req->termListAndStartPoint = ccl_scan_query (out, rpn)))
        {
            printf("Couldn't convert CCL to Scan term\n");
            return -1;
        }
        ccl_rpn_delete (rpn);
    }
#endif
    if (use_rpn && !(req->termListAndStartPoint =
                     p_query_scan(out, protocol, &req->attributeSet, query)))
    {
        printf("Prefix query error\n");
        return -1;
    }
    if (term && *term)
    {
        if (req->termListAndStartPoint->term &&
            req->termListAndStartPoint->term->which == Z_Term_general &&
            req->termListAndStartPoint->term->u.general)
        {
            req->termListAndStartPoint->term->u.general->buf =
                (unsigned char *) odr_strdup(out, term);
            req->termListAndStartPoint->term->u.general->len =
                req->termListAndStartPoint->term->u.general->size =
                strlen(term);
        }
    }
    req->referenceId = set_refid (out);
    req->num_databaseNames = num_databaseNames;
    req->databaseNames = databaseNames;
    req->numberOfTermsRequested = &num;
    req->preferredPositionInResponse = &pp;
    send_apdu(apdu);
    return 2;
}

int send_sortrequest(char *arg, int newset)
{
    Z_APDU *apdu = zget_APDU(out, Z_APDU_sortRequest);
    Z_SortRequest *req = apdu->u.sortRequest;
    Z_SortKeySpecList *sksl = (Z_SortKeySpecList *)
        odr_malloc (out, sizeof(*sksl));
    char setstring[32];
    char sort_string_buf[32], sort_flags[32];
    int off;
    int oid[OID_SIZE];
    oident bib1;

    if (setnumber >= 0)
        sprintf (setstring, "%d", setnumber);
    else
        sprintf (setstring, "default");

    req->referenceId = set_refid (out);

#ifdef ASN_COMPILED
    req->num_inputResultSetNames = 1;
    req->inputResultSetNames = (Z_InternationalString **)
        odr_malloc (out, sizeof(*req->inputResultSetNames));
    req->inputResultSetNames[0] = odr_strdup (out, setstring);
#else
    req->inputResultSetNames =
        (Z_StringList *)odr_malloc (out, sizeof(*req->inputResultSetNames));
    req->inputResultSetNames->num_strings = 1;
    req->inputResultSetNames->strings =
        (char **)odr_malloc (out, sizeof(*req->inputResultSetNames->strings));
    req->inputResultSetNames->strings[0] =
        odr_strdup (out, setstring);
#endif

    if (newset && setnumber >= 0)
        sprintf (setstring, "%d", ++setnumber);

    req->sortedResultSetName = odr_strdup (out, setstring);

    req->sortSequence = sksl;
    sksl->num_specs = 0;
    sksl->specs = (Z_SortKeySpec **)odr_malloc (out, sizeof(sksl->specs) * 20);
    
    bib1.proto = protocol;
    bib1.oclass = CLASS_ATTSET;
    bib1.value = VAL_BIB1;
    while ((sscanf (arg, "%31s %31s%n", sort_string_buf, sort_flags, &off)) == 2 
           && off > 1)
    {
        int i;
        char *sort_string_sep;
	char *sort_string = sort_string_buf;
        Z_SortKeySpec *sks = (Z_SortKeySpec *)odr_malloc (out, sizeof(*sks));
        Z_SortKey *sk = (Z_SortKey *)odr_malloc (out, sizeof(*sk));

        arg += off;
        sksl->specs[sksl->num_specs++] = sks;
        sks->sortElement = (Z_SortElement *)odr_malloc (out, sizeof(*sks->sortElement));
        sks->sortElement->which = Z_SortElement_generic;
        sks->sortElement->u.generic = sk;
        
        if ((sort_string_sep = strchr (sort_string, '=')))
        {
	    int i = 0;
            sk->which = Z_SortKey_sortAttributes;
            sk->u.sortAttributes =
                (Z_SortAttributes *)odr_malloc (out, sizeof(*sk->u.sortAttributes));
            sk->u.sortAttributes->id = oid_ent_to_oid(&bib1, oid);
            sk->u.sortAttributes->list =
                (Z_AttributeList *)odr_malloc (out, sizeof(*sk->u.sortAttributes->list));
            sk->u.sortAttributes->list->attributes =
                (Z_AttributeElement **)
		odr_malloc (out, 10 *
			    sizeof(*sk->u.sortAttributes->list->attributes));
	    while (i < 10 && sort_string && sort_string_sep)
	    {
		Z_AttributeElement *el = (Z_AttributeElement *)
		    odr_malloc (out, sizeof(*el));
		sk->u.sortAttributes->list->attributes[i] = el;
		el->attributeSet = 0;
		el->attributeType = (int *)odr_malloc (out, sizeof(*el->attributeType));
		*el->attributeType = atoi (sort_string);
		el->which = Z_AttributeValue_numeric;
		el->value.numeric = (int *)odr_malloc (out, sizeof(*el->value.numeric));
		*el->value.numeric = atoi (sort_string_sep + 1);
		i++;
		sort_string = strchr(sort_string, ',');
		if (sort_string)
		{
		    sort_string++;
		    sort_string_sep = strchr (sort_string, '=');
		}
	    }
            sk->u.sortAttributes->list->num_attributes = i;
        }
        else
        {
            sk->which = Z_SortKey_sortField;
            sk->u.sortField = odr_strdup (out, sort_string);
        }
        sks->sortRelation = (int *)odr_malloc (out, sizeof(*sks->sortRelation));
        *sks->sortRelation = Z_SortRelation_ascending;
        sks->caseSensitivity = (int *)odr_malloc (out, sizeof(*sks->caseSensitivity));
        *sks->caseSensitivity = Z_SortCase_caseSensitive;

#ifdef ASN_COMPILED
        sks->which = Z_SortKeySpec_null;
        sks->u.null = odr_nullval ();
#else
        sks->missingValueAction = NULL;
#endif

        for (i = 0; sort_flags[i]; i++)
        {
            switch (sort_flags[i])
            {
            case 'a':
            case 'A':
            case '>':
                *sks->sortRelation = Z_SortRelation_descending;
                break;
            case 'd':
            case 'D':
            case '<':
                *sks->sortRelation = Z_SortRelation_ascending;
                break;
            case 'i':
            case 'I':
                *sks->caseSensitivity = Z_SortCase_caseInsensitive;
                break;
            case 'S':
            case 's':
                *sks->caseSensitivity = Z_SortCase_caseSensitive;
                break;
            }
        }
    }
    if (!sksl->num_specs)
    {
        printf ("Missing sort specifications\n");
        return -1;
    }
    send_apdu(apdu);
    return 2;
}

void display_term(Z_TermInfo *t)
{
    if (t->term->which == Z_Term_general)
    {
        printf("%.*s", t->term->u.general->len, t->term->u.general->buf);
        sprintf(last_scan_line, "%.*s", t->term->u.general->len,
            t->term->u.general->buf);
    }
    else
        printf("Term (not general)");
    if (t->globalOccurrences)
        printf (" (%d)\n", *t->globalOccurrences);
    else
        printf ("\n");
}

void process_scanResponse(Z_ScanResponse *res)
{
    int i;
    Z_Entry **entries = NULL;
    int num_entries = 0;
   
    printf("Received ScanResponse\n"); 
    print_refid (res->referenceId);
    printf("%d entries", *res->numberOfEntriesReturned);
    if (res->positionOfTerm)
        printf (", position=%d", *res->positionOfTerm); 
    printf ("\n");
    if (*res->scanStatus != Z_Scan_success)
        printf("Scan returned code %d\n", *res->scanStatus);
    if (!res->entries)
        return;
    if ((entries = res->entries->entries))
        num_entries = res->entries->num_entries;
    for (i = 0; i < num_entries; i++)
    {
        int pos_term = res->positionOfTerm ? *res->positionOfTerm : -1;
        if (entries[i]->which == Z_Entry_termInfo)
        {
            printf("%c ", i + 1 == pos_term ? '*' : ' ');
            display_term(entries[i]->u.termInfo);
        }
        else
            display_diagrecs(&entries[i]->u.surrogateDiagnostic, 1);
    }
    if (res->entries->nonsurrogateDiagnostics)
        display_diagrecs (res->entries->nonsurrogateDiagnostics,
                          res->entries->num_nonsurrogateDiagnostics);
}

void process_sortResponse(Z_SortResponse *res)
{
    printf("Received SortResponse: status=");
    switch (*res->sortStatus)
    {
    case Z_SortStatus_success:
        printf ("success"); break;
    case Z_SortStatus_partial_1:
        printf ("partial"); break;
    case Z_SortStatus_failure:
        printf ("failure"); break;
    default:
        printf ("unknown (%d)", *res->sortStatus);
    }
    printf ("\n");
    print_refid (res->referenceId);
#ifdef ASN_COMPILED
    if (res->diagnostics)
        display_diagrecs(res->diagnostics,
                         res->num_diagnostics);
#else
    if (res->diagnostics)
        display_diagrecs(res->diagnostics->diagRecs,
                         res->diagnostics->num_diagRecs);
#endif
}

void process_deleteResultSetResponse (Z_DeleteResultSetResponse *res)
{
    printf("Got deleteResultSetResponse status=%d\n",
           *res->deleteOperationStatus);
    if (res->deleteListStatuses)
    {
        int i;
        for (i = 0; i < res->deleteListStatuses->num; i++)
        {
            printf ("%s status=%d\n", res->deleteListStatuses->elements[i]->id,
                    *res->deleteListStatuses->elements[i]->status);
        }
    }
}

int cmd_sort_generic(char *arg, int newset)
{
    if (!conn)
    {
        printf("Session not initialized yet\n");
        return 0;
    }
    if (!ODR_MASK_GET(session->options, Z_Options_sort))
    {
        printf("Target doesn't support sort\n");
        return 0;
    }
    if (*arg)
    {
        if (send_sortrequest(arg, newset) < 0)
            return 0;
        return 2;
    }
    return 0;
}

int cmd_sort(char *arg)
{
    return cmd_sort_generic (arg, 0);
}

int cmd_sort_newset (char *arg)
{
    return cmd_sort_generic (arg, 1);
}

int cmd_scan(char *arg)
{
    if (!conn)
    {
        printf("Session not initialized yet\n");
        return 0;
    }
    if (!ODR_MASK_GET(session->options, Z_Options_scan))
    {
        printf("Target doesn't support scan\n");
        return 0;
    }
    if (*arg)
    {
        strcpy (last_scan_query, arg);
        if (send_scanrequest(arg, 1, 20, 0) < 0)
            return 0;
    }
    else
    {
        if (send_scanrequest(last_scan_query, 1, 20, last_scan_line) < 0)
            return 0;
    }
    return 2;
}

int cmd_schema(char *arg)
{
    if (!arg || !*arg)
    {
        schema = VAL_NONE;
        return 1;
    }
    schema = oid_getvalbyname (arg);
    if (schema == VAL_NONE)
    {
        printf ("unknown schema\n");
        return 0;
    }
    return 1;
}

int cmd_format(char *arg)
{
    if (!arg || !*arg)
    {
        printf("Usage: format <recordsyntax>\n");
        return 0;
    }
    recordsyntax = oid_getvalbyname (arg);
    if (recordsyntax == VAL_NONE)
    {
        printf ("unknown record syntax\n");
        return 0;
    }
    return 1;
}

int cmd_elements(char *arg)
{
    static Z_ElementSetNames esn;
    static char what[100];

    if (!arg || !*arg)
    {
        elementSetNames = 0;
        return 1;
    }
    strcpy(what, arg);
    esn.which = Z_ElementSetNames_generic;
    esn.u.generic = what;
    elementSetNames = &esn;
    return 1;
}

int cmd_attributeset(char *arg)
{
    char what[100];

    if (!arg || !*arg)
    {
        printf("Usage: attributeset <setname>\n");
        return 0;
    }
    sscanf(arg, "%s", what);
    if (p_query_attset (what))
    {
        printf("Unknown attribute set name\n");
        return 0;
    }
    return 1;
}

int cmd_querytype (char *arg)
{
    if (!strcmp (arg, "ccl"))
        queryType = QueryType_CCL;
    else if (!strcmp (arg, "prefix") || !strcmp(arg, "rpn"))
        queryType = QueryType_Prefix;
#if YAZ_MODULE_ccl
    else if (!strcmp (arg, "ccl2rpn") || !strcmp (arg, "cclrpn"))
        queryType = QueryType_CCL2RPN;
#endif
    else
    {
        printf ("Querytype must be one of:\n");
        printf (" prefix         - Prefix query\n");
        printf (" ccl            - CCL query\n");
#if YAZ_MODULE_ccl
        printf (" ccl2rpn        - CCL query converted to RPN\n");
#endif
        return 0;
    }
    return 1;
}

int cmd_refid (char *arg)
{
    xfree (refid);
    refid = NULL;
    if (*arg)
    {
        refid = (char *) xmalloc (strlen(arg)+1);
        strcpy (refid, arg);
    }
    return 1;
}

int cmd_close(char *arg)
{
    Z_APDU *apdu;
    Z_Close *req;
    if (!conn)
        return 0;

    apdu = zget_APDU(out, Z_APDU_close);
    req = apdu->u.close;
    *req->closeReason = Z_Close_finished;
    send_apdu(apdu);
    printf("Sent close request.\n");
    sent_close = 1;
    return 2;
}

int cmd_packagename(char* arg) {
    xfree (esPackageName);
    esPackageName = NULL;
    if (*arg)
    {
        esPackageName = (char *) xmalloc (strlen(arg)+1);
        strcpy (esPackageName, arg);
    }
    return 1;
};

void initialize(void)
{
#if YAZ_MODULE_ccl
    FILE *inf;
#endif
    nmem_init();
    if (!(out = odr_createmem(ODR_ENCODE)) ||
        !(in = odr_createmem(ODR_DECODE)) ||
        !(print = odr_createmem(ODR_PRINT)))
    {
        fprintf(stderr, "failed to allocate ODR streams\n");
        exit(1);
    }
    setvbuf(stdout, 0, _IONBF, 0);
    if (apdu_file)
        odr_setprint(print, apdu_file);

#if YAZ_MODULE_ccl
    bibset = ccl_qual_mk (); 
    inf = fopen (ccl_fields, "r");
    if (inf)
    {
        ccl_qual_file (bibset, inf);
        fclose (inf);
    }
#endif
    cmd_base("Default");
}

static int client(int wait)
{
    static struct {
        char *cmd;
        int (*fun)(char *arg);
        char *ad;
    } cmd[] = {
        {"open", cmd_open, "('tcp'|'osi')':'[<tsel>'/']<host>[':'<port>]"},
        {"quit", cmd_quit, ""},
        {"find", cmd_find, "<query>"},
        {"delete", cmd_delete, "<setname>"},
        {"base", cmd_base, "<base-name>"},
        {"show", cmd_show, "<rec#>['+'<#recs>['+'<setname>]]"},
        {"scan", cmd_scan, "<term>"},
        {"sort", cmd_sort, "<sortkey> <flag> <sortkey> <flag> ..."},
        {"sort+", cmd_sort_newset, "<sortkey> <flag> <sortkey> <flag> ..."},
        {"authentication", cmd_authentication, "<acctstring>"},
        {"lslb", cmd_lslb, "<largeSetLowerBound>"},
        {"ssub", cmd_ssub, "<smallSetUpperBound>"},
        {"mspn", cmd_mspn, "<mediumSetPresentNumber>"},
        {"status", cmd_status, ""},
        {"setnames", cmd_setnames, ""},
        {"cancel", cmd_cancel, ""},
        {"format", cmd_format, "<recordsyntax>"},
        {"schema", cmd_schema, "<schema>"},
        {"elements", cmd_elements, "<elementSetName>"},
        {"close", cmd_close, ""},
        {"attributeset", cmd_attributeset, "<attrset>"},
        {"querytype", cmd_querytype, "<type>"},
        {"refid", cmd_refid, "<id>"},
        {"itemorder", cmd_itemorder, "ill|item <itemno>"},
        {"update", cmd_update, "<item>"},
	{"packagename", cmd_packagename, "<packagename>"},
#if 0
#ifdef ASN_COMPILED
        /* Server Admin Functions */
        {"adm-reindex", cmd_adm_reindex, "<database-name>"},
        {"adm-truncate", cmd_adm_truncate, "('database'|'index')<object-name>"},
        {"adm-create", cmd_adm_create, ""},
        {"adm-drop", cmd_adm_drop, "('database'|'index')<object-name>"},
        {"adm-import", cmd_adm_import, "<record-type> <dir> <pattern>"},
        {"adm-refresh", cmd_adm_refresh, ""},
        {"adm-commit", cmd_adm_commit, ""},
        {"adm-shutdown", cmd_adm_shutdown, ""},
        {"adm-startup", cmd_adm_startup, ""},
#endif
#endif
        {0,0}
    };
    char *netbuffer= 0;
    int netbufferlen = 0;
    int i;
    Z_APDU *apdu;
#if HAVE_GETTIMEOFDAY
    struct timeval tv_start, tv_end;
    gettimeofday (&tv_start, 0);
#endif

    while (1)
    {
        int res;
#ifdef USE_SELECT
        fd_set input;
#endif
        char line[1024], word[1024], arg[1024];
        
#ifdef USE_SELECT
        FD_ZERO(&input);
        FD_SET(0, &input);
        if (conn)
            FD_SET(cs_fileno(conn), &input);
        if ((res = select(20, &input, 0, 0, 0)) < 0)
        {
            perror("select");
            exit(1);
        }
        if (!res)
            continue;
        if (!wait && FD_ISSET(0, &input))
#else
        if (!wait)
#endif
        {
#if HAVE_READLINE_READLINE_H
            char* line_in;
            line_in=readline(C_PROMPT);
            if (!line_in)
                break;
#if HAVE_READLINE_HISTORY_H
            if (*line_in)
                add_history(line_in);
#endif
            strcpy(line,line_in);
            free (line_in);
#else    
            char *end_p;
            printf (C_PROMPT);
            fflush(stdout);
            if (!fgets(line, 1023, stdin))
                break;
            if ((end_p = strchr (line, '\n')))
                *end_p = '\0';
#endif 
#if HAVE_GETTIMEOFDAY
            gettimeofday (&tv_start, 0);
#endif

            if ((res = sscanf(line, "%s %[^;]", word, arg)) <= 0)
            {
                strcpy(word, last_cmd);
                *arg = '\0';
            }
            else if (res == 1)
                *arg = 0;
            strcpy(last_cmd, word);
            for (i = 0; cmd[i].cmd; i++)
                if (!strncmp(cmd[i].cmd, word, strlen(word)))
                {
                    res = (*cmd[i].fun)(arg);
                    break;
                }
            if (!cmd[i].cmd) /* dump our help-screen */
            {
                printf("Unknown command: %s.\n", word);
                printf("Currently recognized commands:\n");
                for (i = 0; cmd[i].cmd; i++)
                    printf("   %s %s\n", cmd[i].cmd, cmd[i].ad);
                res = 1;
            }
            if (res < 2)
            {
                continue;
            }
        }
        wait = 0;
        if (conn
#ifdef USE_SELECT
            && FD_ISSET(cs_fileno(conn), &input)
#endif
            )
        {
            do
            {
                if ((res = cs_get(conn, &netbuffer, &netbufferlen)) < 0)
                {
                    perror("cs_get");
                    exit(1);
                }
                if (!res)
                {
                    printf("Target closed connection.\n");
                    exit(1);
                }
                odr_reset(in); /* release APDU from last round */
                record_last = 0;
                odr_setbuf(in, netbuffer, res, 0);
                if (!z_APDU(in, &apdu, 0, 0))
                {
                    odr_perror(in, "Decoding incoming APDU");
                    fprintf(stderr, "[Near %d]\n", odr_offset(in));
                    fprintf(stderr, "Packet dump:\n---------\n");
                    odr_dumpBER(stderr, netbuffer, res);
                    fprintf(stderr, "---------\n");
                    if (apdu_file)
                        z_APDU(print, &apdu, 0, 0);
                    exit(1);
                }
                if (apdu_file && !z_APDU(print, &apdu, 0, 0))
                {
                    odr_perror(print, "Failed to print incoming APDU");
                    odr_reset(print);
                    continue;
                }
                switch(apdu->which)
                {
                case Z_APDU_initResponse:
                    process_initResponse(apdu->u.initResponse);
                    break;
                case Z_APDU_searchResponse:
                    process_searchResponse(apdu->u.searchResponse);
                    break;
                case Z_APDU_scanResponse:
                    process_scanResponse(apdu->u.scanResponse);
                    break;
                case Z_APDU_presentResponse:
                    print_refid (apdu->u.presentResponse->referenceId);
                    setno +=
                        *apdu->u.presentResponse->numberOfRecordsReturned;
                    if (apdu->u.presentResponse->records)
                        display_records(apdu->u.presentResponse->records);
                    else
                        printf("No records.\n");
                    printf ("nextResultSetPosition = %d\n",
                        *apdu->u.presentResponse->nextResultSetPosition);
                    break;
                case Z_APDU_sortResponse:
                    process_sortResponse(apdu->u.sortResponse);
                    break;
                case Z_APDU_extendedServicesResponse:
                    printf("Got extended services response\n");
                    process_ESResponse(apdu->u.extendedServicesResponse);
                    break;
                case Z_APDU_close:
                    printf("Target has closed the association.\n");
                    process_close(apdu->u.close);
                    break;
                case Z_APDU_resourceControlRequest:
                    process_resourceControlRequest
                        (apdu->u.resourceControlRequest);
                    break;
                case Z_APDU_deleteResultSetResponse:
                    process_deleteResultSetResponse(apdu->u.
                                                    deleteResultSetResponse);
                    break;
                default:
                    printf("Received unknown APDU type (%d).\n", 
                           apdu->which);
                    exit(1);
                }
            }
            while (conn && cs_more(conn));
#if HAVE_GETTIMEOFDAY
            gettimeofday (&tv_end, 0);
            if (1)
            {
                printf ("Elapsed: %.6f\n", (double) tv_end.tv_usec /
                                                1e6 + tv_end.tv_sec -
                   ((double) tv_start.tv_usec / 1e6 + tv_start.tv_sec));
            }
#endif
        }
    }
    return 0;
}

#ifdef DEBUG
int main(int argc, char **argv)
{
    char *prog = *argv;
    char *arg;
    int ret;
    int opened = 0;

    while ((ret = options("c:a:m:v:", argv, argc, &arg)) != -2)
    {
        switch (ret)
        {
        case 0:
            if (!opened)
            {
                initialize ();
                if (cmd_open (arg) == 2) {
#if HAVE_READLINE_HISTORY_H
		  char* tmp=(char*)malloc(strlen(arg)+6);
		  *tmp=0;
		  strcat(tmp,"open ");
		  strcat(tmp,arg);
		  add_history(tmp);
		  free(tmp);
#endif
		  opened = 1;
		};
            }
            break;
        case 'm':
            if (!(marcdump = fopen (arg, "a")))
            {
                perror (arg);
                exit (1);
            }
            break;
	case 'c':
	    strncpy (ccl_fields, arg, sizeof(ccl_fields)-1);
	    ccl_fields[sizeof(ccl_fields)-1] = '\0';
	    break;
        case 'a':
            if (!strcmp(arg, "-"))
                apdu_file=stderr;
            else
                apdu_file=fopen(arg, "a");
            break;
        case 'v':
            yaz_log_init (yaz_log_mask_str(arg), "", NULL);
            break;
        default:
            fprintf (stderr, "Usage: %s [-m <marclog>] [ -a <apdulog>] "
                             "[-c cclfields] [<server-addr>]\n",
                     prog);
            exit (1);
        }
    }
    if (!opened)
        initialize ();
    return client (opened);
}
#endif

int z3950_callback(char* word, char* arg)
{
    static struct {
        char *cmd;
        int (*fun)(char *arg);
        char *ad;
    } cmd[] = {
        {"open", cmd_open, "('tcp'|'osi')':'[<tsel>'/']<host>[':'<port>]"},
        {"quit", cmd_quit, ""},
        {"find", cmd_find, "<query>"},
        {"delete", cmd_delete, "<setname>"},
        {"base", cmd_base, "<base-name>"},
        {"show", cmd_show, "<rec#>['+'<#recs>['+'<setname>]]"},
        {"scan", cmd_scan, "<term>"},
        {"sort", cmd_sort, "<sortkey> <flag> <sortkey> <flag> ..."},
        {"sort+", cmd_sort_newset, "<sortkey> <flag> <sortkey> <flag> ..."},
        {"authentication", cmd_authentication, "<acctstring>"},
        {"lslb", cmd_lslb, "<largeSetLowerBound>"},
        {"ssub", cmd_ssub, "<smallSetUpperBound>"},
        {"mspn", cmd_mspn, "<mediumSetPresentNumber>"},
        {"status", cmd_status, ""},
        {"setnames", cmd_setnames, ""},
        {"cancel", cmd_cancel, ""},
        {"format", cmd_format, "<recordsyntax>"},
        {"schema", cmd_schema, "<schema>"},
        {"elements", cmd_elements, "<elementSetName>"},
        {"close", cmd_close, ""},
        {"attributeset", cmd_attributeset, "<attrset>"},
        {"querytype", cmd_querytype, "<type>"},
        {"refid", cmd_refid, "<id>"},
        {"itemorder", cmd_itemorder, "ill|item <itemno>"},
        {"update", cmd_update, "<item>"},
	{"packagename", cmd_packagename, "<packagename>"},
#if 0
#ifdef ASN_COMPILED
        /* Server Admin Functions */
        {"adm-reindex", cmd_adm_reindex, "<database-name>"},
        {"adm-truncate", cmd_adm_truncate, "('database'|'index')<object-name>"},
        {"adm-create", cmd_adm_create, ""},
        {"adm-drop", cmd_adm_drop, "('database'|'index')<object-name>"},
        {"adm-import", cmd_adm_import, "<record-type> <dir> <pattern>"},
        {"adm-refresh", cmd_adm_refresh, ""},
        {"adm-commit", cmd_adm_commit, ""},
        {"adm-shutdown", cmd_adm_shutdown, ""},
        {"adm-startup", cmd_adm_startup, ""},
#endif
#endif
        {0,0}
    };
    char *netbuffer= 0;
    int netbufferlen = 0;
    int i;
    Z_APDU *apdu;
#if HAVE_GETTIMEOFDAY
    struct timeval tv_start, tv_end;
    gettimeofday (&tv_start, 0);
#endif

        int res;
#if HAVE_GETTIMEOFDAY
            gettimeofday (&tv_start, 0);
#endif
            for (i = 0; cmd[i].cmd; i++)
                if (!strncmp(cmd[i].cmd, word, strlen(word)))
                {
                    res = (*cmd[i].fun)(arg);
                    break;
                }
            if (!cmd[i].cmd)
            {
                printf("Unknown command: %s.\n", word);
                res = 1;
            }
            if (res < 2)
            {
                return 1;
            }
        if (conn)
        {
            do
            {
                if ((res = cs_get(conn, &netbuffer, &netbufferlen)) < 0)
                {
                    perror("cs_get");
                    exit(1);
                }
                if (!res)
                {
                    printf("Target closed connection.\n");
                    exit(1);
                }
                odr_reset(in); /* release APDU from last round */
                record_last = 0;
                odr_setbuf(in, netbuffer, res, 0);
                if (!z_APDU(in, &apdu, 0, 0))
                {
                    odr_perror(in, "Decoding incoming APDU");
                    fprintf(stderr, "[Near %d]\n", odr_offset(in));
                    fprintf(stderr, "Packet dump:\n---------\n");
                    odr_dumpBER(stderr, netbuffer, res);
                    fprintf(stderr, "---------\n");
                    if (apdu_file)
                        z_APDU(print, &apdu, 0, 0);
                    exit(1);
                }
                if (apdu_file && !z_APDU(print, &apdu, 0, 0))
                {
                    odr_perror(print, "Failed to print incoming APDU");
                    odr_reset(print);
                    continue;
                }
                switch(apdu->which)
                {
                case Z_APDU_initResponse:
                    process_initResponse(apdu->u.initResponse);
                    break;
                case Z_APDU_searchResponse:
                    process_searchResponse(apdu->u.searchResponse);
                    break;
                case Z_APDU_scanResponse:
                    process_scanResponse(apdu->u.scanResponse);
                    break;
                case Z_APDU_presentResponse:
                    print_refid (apdu->u.presentResponse->referenceId);
                    setno +=
                        *apdu->u.presentResponse->numberOfRecordsReturned;
                    if (apdu->u.presentResponse->records)
                        display_records(apdu->u.presentResponse->records);
                    else
                        printf("No records.\n");
                    printf ("nextResultSetPosition = %d\n",
                        *apdu->u.presentResponse->nextResultSetPosition);
                    break;
                case Z_APDU_sortResponse:
                    process_sortResponse(apdu->u.sortResponse);
                    break;
                case Z_APDU_extendedServicesResponse:
                    printf("Got extended services response\n");
                    process_ESResponse(apdu->u.extendedServicesResponse);
                    break;
                case Z_APDU_close:
                    printf("Target has closed the association.\n");
                    process_close(apdu->u.close);
                    break;
                case Z_APDU_resourceControlRequest:
                    process_resourceControlRequest
                        (apdu->u.resourceControlRequest);
                    break;
                case Z_APDU_deleteResultSetResponse:
                    process_deleteResultSetResponse(apdu->u.
                                                    deleteResultSetResponse);
                    break;
                default:
                    printf("Received unknown APDU type (%d).\n", 
                           apdu->which);
                    exit(1);
                }
            }
            while (conn && cs_more(conn));
#if HAVE_GETTIMEOFDAY
            gettimeofday (&tv_end, 0);
            if (1)
            {
                printf ("Elapsed: %.6f\n", (double) tv_end.tv_usec /
                                                1e6 + tv_end.tv_sec -
                   ((double) tv_start.tv_usec / 1e6 + tv_start.tv_sec));
            }
#endif
	}
    return 0;
}
