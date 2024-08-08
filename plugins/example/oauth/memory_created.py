import requests
from fastapi import HTTPException, Request, Form, APIRouter
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates

import templates as templates
from db import *
from models import Memory, EndpointResponse

from oauth_client import getNotion

router = APIRouter()
# noinspection PyRedeclaration
templates = Jinja2Templates(directory="templates/oauth")


@router.get('/notion/setup-notion-crm', response_class=HTMLResponse, tags=['oauth'])
async def setup_notion_crm(request: Request, uid: str):
    """
    Simple setup page Form page for Notion CRM plugin.
    """
    if not uid:
        raise HTTPException(status_code=400, detail='UID is required')
    oauth_url = getNotion().getOAuthUrl(uid)
    return templates.TemplateResponse("setup_notion_crm.html", {"request": request, "uid": uid, "oauth_url": oauth_url})


def response_setup_notion_crm_page(request: Request, uid: str, err: str):
    if not uid:
        raise HTTPException(status_code=400, detail='UID is required')
    oauth_url = getNotion().getOAuthUrl(uid)
    return templates.TemplateResponse("setup_notion_crm.html", {
        "request": request, "uid": uid,
        "oauth_url": oauth_url,
        "error_message": err if err != "" else None,
    })


@router.get('/auth/notion/callback', response_class=HTMLResponse, tags=['oauth'])
async def callback_auth_notion_crm(request: Request, state: str, code: str):
    """
    Callback from Notion Oauth.
    """

    uid = state

    # Get access token
    oauthOk = getNotion().getAccessToken(code)
    if "error" in oauthOk:
        err = oauthOk["error"]
        print(err)
        return response_setup_notion_crm_page(request, uid, f"Something went wrong. Please try again! \n (code: 400001)")

    oauth = oauthOk["result"]

    # Validate access token
    access_token = oauth.access_token
    if oauth.access_token == "":
        return response_setup_notion_crm_page(request, uid, f"Something went wrong. Please try again! \n (code: 400002)")

    # Get database to create creds_notion_crm
    databasesOk = getNotion().getDatabasesEditedTimeDesc(access_token)
    if "error" in databasesOk:
        err = databasesOk["error"]
        print(err)
        return response_setup_notion_crm_page(request, uid, f"Something went wrong. Please try again! \n (code: 400003)")

    # Pick top
    databases = databasesOk["result"]
    if len(databases) == 0 or databases[0].id == "":
        return response_setup_notion_crm_page(request, uid, f"There is no database. Please try again!  \n (code: 400004)")
    database_id = databases[0].id

    # Save
    print({'uid': uid, 'api_key': access_token, 'database_id': database_id})
    store_notion_crm_api_key(uid, access_token)
    store_notion_database_id(uid, database_id)
    return templates.TemplateResponse("okpage.html", {"request": request, "uid": uid})


@router.post('/notion/creds/notion-crm', response_class=HTMLResponse, tags=['oauth'])
def creds_notion_crm(request: Request, uid: str = Form(...), api_key: str = Form(...), database_id: str = Form(...)):
    """
    Store the Notion CRM API Key and Database ID in redis "authenticate the user".
    This endpoint gets called from /setup-notion-crm page.
    Parameters
    ----------
    request: Request -> FastAPI Request object
    uid: str -> User ID from the query parameter
    api_key: str -> Notion Integration created API key.
    database_id: str -> Notion Database ID where the data will be stored.

    """
    if not api_key or not database_id:
        raise HTTPException(
            status_code=400, detail='API Key and Database ID are required')
    print({'uid': uid, 'api_key': api_key, 'database_id': database_id})
    store_notion_crm_api_key(uid, api_key)
    store_notion_database_id(uid, database_id)
    return templates.TemplateResponse("okpage.html", {"request": request, "uid": uid})


@router.get('/notion/setup/notion-crm', tags=['oauth'])
def is_setup_completed(uid: str):
    """
    Check if the user has setup the Notion CRM plugin.
    """
    notion_api_key = get_notion_crm_api_key(uid)
    notion_database_id = get_notion_database_id(uid)
    return {'is_setup_completed': notion_api_key is not None and notion_database_id is not None}


@router.post('/notion/notion-crm', tags=['oauth', 'memory_created'], response_model=EndpointResponse)
def notion_crm(memory: Memory, uid: str):
    """
    The actual plugin that gets triggered when a memory gets created, and adds the memory to the Notion CRM.
    """
    notion_api_key = get_notion_crm_api_key(uid)
    if not notion_api_key:
        return {'message': 'Your Notion CRM plugin is not setup properly. Check your plugin settings.'}

    create_notion_row(notion_api_key, get_notion_database_id(uid), memory)

    return {}


def create_notion_row(notion_api_key: str, database_id: str, memory: Memory):
    # Validate table exists and has correct fields
    databaseOk = getNotion().getDatabase(database_id, notion_api_key)
    if "error" in databaseOk:
        err = databaseOk["error"]
        raise HTTPException(
            status_code=400, detail=f"Something went wrong.\n{err}")
        return

    # Use set to optimize exists validating
    property_set = set()
    for field in databaseOk["result"].properties:
        property_set.add(field.name)

    # Collect all miss fields
    missing_fields = []
    for field in ["Title", "Category", "Overview", "Speakers", "Duration (seconds)"]:
        if field not in property_set:
            missing_fields.append(field)

    # If any missing, raise error
    if len(missing_fields) > 0:
        value = ", ".join(missing_fields)
        raise HTTPException(
            status_code=400, detail=f"Fields are missing: {value}")
        return

    # Create row
    try:
        emoji = memory.structured.emoji.encode('latin1').decode('utf-8')
    except UnicodeEncodeError:
        emoji = memory.structured.emoji

    data = {
        "parent": {"database_id": database_id},
        "icon": {
            "type": "emoji",
            "emoji": f"{emoji}"
        },
        "properties": {
            "Title": {"title": [{"text": {"content": f'{memory.structured.title}'}}]},
            "Category": {"select": {"name": memory.structured.category}},
            "Overview": {"rich_text": [{"text": {"content": memory.structured.overview}}]},
            "Speakers": {'number': len(set(map(lambda x: x.speaker, memory.transcript_segments)))},
            "Duration (seconds)": {'number': (
                memory.finished_at - memory.started_at).total_seconds() if memory.finished_at is not None else 0},
        }
    }
    resp = requests.post('https://api.notion.com/v1/pages', json=data, headers={
        'Authorization': f'Bearer {notion_api_key}',
        'Content-Type': 'application/json',
        'Accept': 'application/json',
        'Notion-Version': '2022-06-28'
    })
    print(resp.json())
    # TODO: after, write inside the page the transcript and everything else.
    return resp.status_code == 200
