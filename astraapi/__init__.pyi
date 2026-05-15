"""AstraAPI framework — typed stub for static analysis."""

from astraapi._status import status as status
from astraapi.applications import AstraAPI as AstraAPI
from astraapi.background import BackgroundTasks as BackgroundTasks
from astraapi.datastructures import UploadFile as UploadFile
from astraapi.exceptions import HTTPException as HTTPException
from astraapi.exceptions import WebSocketException as WebSocketException
from astraapi.param_functions import Body as Body
from astraapi.param_functions import Cookie as Cookie
from astraapi.param_functions import Depends as Depends
from astraapi.param_functions import File as File
from astraapi.param_functions import Form as Form
from astraapi.param_functions import Header as Header
from astraapi.param_functions import Path as Path
from astraapi.param_functions import Query as Query
from astraapi.param_functions import Security as Security
from astraapi.requests import Request as Request
from astraapi.responses import Response as Response
from astraapi.routing import APIRouter as APIRouter
from astraapi.websockets import WebSocket as WebSocket
from astraapi.websockets import WebSocketDisconnect as WebSocketDisconnect

__version__: str
