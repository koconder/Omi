import os
from datetime import datetime
from typing import List

from pinecone import Pinecone

from models.memory import Memory
from utils.llm import embeddings

if os.getenv('PINECONE_API_KEY') is not None:
    pc = Pinecone(api_key=os.getenv('PINECONE_API_KEY', ''))
    index = pc.Index(os.getenv('PINECONE_INDEX_NAME', ''))
else:
    index = None


def _get_data(uid: str, memory_id: str, vector: List[float], transcript: str, summary: str):
    return {
        "id": f'{uid}-{memory_id}',
        "values": vector,
        'metadata': {
            'uid': uid,
            'memory_id': memory_id,
            'transcript': transcript,
            'summary': summary,
            # TODO: should store more raw fields? or any at all?
            'created_at': datetime.utcnow().timestamp() / 1000,
        }
    }


def upsert_vector(uid: str, memory: Memory, vector: List[float]):
    res = index.upsert(
        vectors=[_get_data(uid, memory.id, vector, memory.get_transcript(), str(memory.structured))], namespace="ns1"
    )
    print('upsert_vector', res)


def upsert_vectors(
        uid: str, vectors: List[List[float]], memories: List[Memory]
):
    data = [
        _get_data(uid, memory.id, vector, memory.transcript, str(memory.structured)) for memory, vector in
        zip(memories, vectors)
    ]
    res = index.upsert(vectors=data, namespace="ns1")
    print('upsert_vectors', res)


def query_vectors(query: str, uid: str, starts_at: int = None, ends_at: int = None) -> List[str]:
    filter_data = {'uid': uid}
    if starts_at is not None:
        filter_data['created_at'] = {'$gte': starts_at, '$lte': ends_at}

    # print('filter_data', filter_data)
    xq = embeddings.embed_query(query)
    xc = index.query(vector=xq, top_k=5, include_metadata=False, filter=filter_data, namespace="ns1")
    # print(xc)
    return [item['id'].replace(f'{uid}-', '') for item in xc['matches']]


def delete_vector(memory_id: str):
    index.delete(ids=[memory_id], namespace="ns1")
