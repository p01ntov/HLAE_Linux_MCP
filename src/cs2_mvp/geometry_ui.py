"""Explicit UI diagnostics on the launcher worker, outside the game frame."""
from __future__ import annotations


def service_validation(client, demo, game, handled=0):
    state = client.request('get_state')
    request = state.get('geometry_validation_request_id', 0)
    if type(request) is not int or request <= handled:
        return handled
    revision = state['revision']
    try:
        from .camera_guard import validate_camera_path
        result = validate_camera_path(client, project=state['project'], demo=demo, game_dir=game)
    except (OSError, ValueError, ImportError) as error:
        result = {'passed': False, 'status': 'unknown', 'reason': str(error)[:1024],
                  'complete': False, 'static_clear': False}
    client.request('set_geometry_validation', {'request_id': request, 'expected_revision': revision, 'result': result})
    return request
