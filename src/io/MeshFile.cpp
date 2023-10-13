#include "io/MeshFile.h"
#include "gamemanager.h"

#include <fsengine/bsa.h>
#include <half.h>

#include <QBuffer>
#include <QFile>
#include "fp32vec4.hpp"

double snormToDouble(int16_t x) { return x < 0 ? x / double(32768) : x / double(32767); }

Vector3 UnpackUDEC3(quint32 n, bool& invertBitangent)
{
	FloatVector4  v(FloatVector4::convertX10Y10Z10(n));
	invertBitangent = bool((n >> 31) & 1);

	return Vector3(v[0], v[1], v[2]);
}

MeshFile::MeshFile(const QString& filepath)
{
	path = QDir::fromNativeSeparators(filepath.toLower()).toStdString();
	if ( path.size() == 0 )
		return;

	if ( readBytes(QString::fromStdString(path), data) && readMesh() > 0 ) {
		qDebug() << "MeshFile created for" << filepath;
	} else {
		qWarning() << "MeshFile creation failed for" << filepath;
	}
}

bool MeshFile::isValid()
{
	return !data.isNull();
}

quint32 MeshFile::readMesh()
{
	if ( data.isEmpty() )
		return 0;

	QBuffer f(&data);
	if ( f.open(QIODevice::ReadOnly) ) {
		in.setDevice(&f);
		in.setByteOrder(QDataStream::LittleEndian);
		in.setFloatingPointPrecision(QDataStream::SinglePrecision);

		quint32 magic;
		in >> magic;
		if ( magic != 1 )
			return 0;

		quint32 indicesSize;
		in >> indicesSize;
		triangles.resize(indicesSize / 3);

		for ( int i = 0; i < indicesSize / 3; i++ ) {
			Triangle tri;
			in >> tri;
			triangles[i] = tri;

		}

		float scale;
		in >> scale;
		if ( scale <= 0.0 )
			return 0; // From RE

		quint32 numWeightsPerVertex;
		in >> numWeightsPerVertex;
		weightsPerVertex = numWeightsPerVertex;

		quint32 numPositions;
		in >> numPositions;
		positions.resize(numPositions + positions.count());

		for ( int i = 0; i < positions.count(); i++ ) {
			int16_t x, y, z;

			in >> x;
			in >> y;
			in >> z;

			// Dividing by 1024 is near exact previous game scale
			// SNORM is / ~32768 though so scale is 32x smaller in .mesh
			positions[i] = Vector3(snormToDouble(x), snormToDouble(y), snormToDouble(z)) * scale;
		}

		quint32 numCoord1;
		in >> numCoord1;
		coords.append(TexCoords());
		coords[0].resize(numCoord1);

		for ( int i = 0; i < coords[0].count(); i++ ) {
			uint32_t uv;

			in >> uv;
      FloatVector4  uv_f(FloatVector4::convertFloat16(uv));

			Vector2 coord;
			coord[0] = uv_f[0];
			coord[1] = uv_f[1];

			coords[0][i] = coord;
		}

		quint32 numCoord2;
		in >> numCoord2;
		coords.append(TexCoords());
		coords[1].resize(numCoord2);

		for ( int i = 0; i < coords[1].count(); i++ ) {
			uint32_t uv;

			in >> uv;
      FloatVector4  uv_f(FloatVector4::convertFloat16(uv));

			Vector2 coord;
			coord[0] = uv_f[0];
			coord[1] = uv_f[1];

      coords[1][i] = coord;
		}

		quint32 numColor;
		in >> numColor;
		if ( numColor > 0 ) {
			colors.resize(numColor + colors.count());
		}
		for ( int i = 0; i < numColor; i++ ) {
			uint8_t r, g, b, a;
			in >> b;
			in >> g;
			in >> r;
			in >> a;
			colors[i] = Color4(r / 255.0, g / 255.0, b / 255.0, a / 255.0);
		}

		quint32 numNormal;
		in >> numNormal;
		if ( numNormal > 0 ) {
			normals.resize(numNormal + normals.count());
		}
		for ( int i = 0; i < normals.count(); i++ ) {
			quint32 n;
			in >> n;
			bool b = false;
			normals[i] = UnpackUDEC3(n, b);
		}

		quint32 numTangent;
		in >> numTangent;
		if ( numTangent > 0 ) {
			tangents.resize(numTangent + tangents.count());
			tangentsBasis.resize(numTangent + tangentsBasis.count());
			bitangents.resize(numTangent + bitangents.count());
		}
		for ( int i = 0; i < tangents.count(); i++ ) {
			quint32 n;
			in >> n;
			bool b = false;
			auto tan = UnpackUDEC3(n, b);
			tangents[i] = tan;
			// For export
			tangentsBasis[i] = Vector4(tan[0], tan[1], tan[2], (b) ? 1.0 : -1.0);
			bitangents[i] = (b) ? Vector3::crossproduct(normals[i], tangents[i]) : Vector3::crossproduct(tangents[i], normals[i]);
		}

		quint32 numWeights;
		in >> numWeights;
		if ( numWeights > 0 && numWeightsPerVertex > 0 ) {
			weights.resize(numWeights / numWeightsPerVertex);
		}
		for ( int i = 0; i < weights.count(); i++ ) {
			QVector<QPair<quint16, quint16>> weightsUNORM;
			for ( int j = 0; j < 8; j++ ) {
				if ( j < numWeightsPerVertex ) {
					quint16 b, w;
					in >> b;
					in >> w;
					weightsUNORM.append({ b, w });
				} else {
					weightsUNORM.append({0, 0});
				}
			}
			weights[i] = BoneWeightsUNorm(weightsUNORM, i);
		}

		quint32 numLODs;
		in >> numLODs;
		lods.resize(numLODs);
		for ( int i = 0; i < numLODs; i++ ) {
			quint32 indicesSize2;
			in >> indicesSize2;
			lods[i].resize(indicesSize2 / 3);

			for ( int j = 0; j < indicesSize2 / 3; j++ ) {
				Triangle tri;
				in >> tri;
				lods[i][j] = tri;
			}
		}

		return numPositions;
	}

	return 0;
}
